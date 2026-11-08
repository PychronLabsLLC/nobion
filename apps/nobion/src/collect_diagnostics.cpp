#include "collect_diagnostics.hpp"

#include <exception>
#include <mutex>
#include <string>
#include <utility>

#include <QAbstractButton>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QUrl>
#include <QWidget>

#include "alarm_dock.hpp"
#include "core_bridge.hpp"
#include "experiment_bridge.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/process_log.hpp"
#include "nobion/setup/support_bundle.hpp"

namespace nobion::ui {

namespace {

QString q(const std::string& s) { return QString::fromStdString(s); }
// UTF-8 whatever the platform (path::string() throws on Windows for a name it cannot convert).
QString from_path(const std::filesystem::path& p) { return QString::fromStdU16String(p.generic_u16string()); }

const char* severity_name(AlarmSeverity s) {
  switch (s) {
    case AlarmSeverity::Info:
      return "info";
    case AlarmSeverity::Warning:
      return "warning";
    case AlarmSeverity::Critical:
      return "critical";
  }
  return "unknown";
}

QString reveal_label() {
#if defined(Q_OS_MACOS)
  return QObject::tr("Show in Finder");
#elif defined(Q_OS_WIN)
  return QObject::tr("Show in Explorer");
#else
  return QObject::tr("Open Folder");
#endif
}

}  // namespace

std::string live_state_json(const LiveSources& sources) {
  QJsonObject root;
  root.insert(QStringLiteral("app"), QStringLiteral("nobion"));
  QJsonObject transports;
  if (sources.core != nullptr) {
    const auto& state = sources.core->state();
    for (const auto& [name, health] : state.health) {
      QJsonObject t;
      t.insert(QStringLiteral("connected"), health.connected);
      t.insert(QStringLiteral("error_count"), static_cast<double>(health.error_count));
      t.insert(QStringLiteral("last_error"), q(health.last_error));
      transports.insert(q(name), t);
    }
    QJsonObject pressures;
    for (const auto& [name, value] : state.pressures) {
      QJsonObject p;
      p.insert(QStringLiteral("value"), value);
      if (const auto unit = state.units.find(name); unit != state.units.end()) {
        p.insert(QStringLiteral("unit"), q(unit->second));
      }
      pressures.insert(q(name), p);
    }
    root.insert(QStringLiteral("pressures"), pressures);
    QJsonArray pending;
    for (const auto& name : state.pending) pending.append(q(name));
    root.insert(QStringLiteral("pending_actuations"), pending);
    root.insert(QStringLiteral("line_started"), state.started);
  }
  root.insert(QStringLiteral("transports"), transports);
  if (sources.alarms != nullptr) {
    QJsonArray alarms;
    for (const auto& row : sources.alarms->rows()) {
      QJsonObject a;
      a.insert(QStringLiteral("source"), q(row.source));
      a.insert(QStringLiteral("severity"), QString::fromLatin1(severity_name(row.severity)));
      a.insert(QStringLiteral("message"), q(row.message));
      a.insert(QStringLiteral("time"), row.time.toUTC().toString(Qt::ISODate));
      alarms.append(a);
    }
    root.insert(QStringLiteral("alarms"), alarms);
  }
  if (sources.experiment != nullptr) {
    const auto& session = sources.experiment->session();
    QJsonObject executor;
    executor.insert(QStringLiteral("state"), q(std::string(experiment::executor::to_string(session.state()))));
    executor.insert(QStringLiteral("running"), session.running());
    executor.insert(QStringLiteral("pending_saves"), static_cast<double>(session.pending_saves()));
    if (const auto error = session.last_save_error()) executor.insert(QStringLiteral("last_save_error"), q(*error));
    QJsonArray problems;
    for (const auto& p : session.problems()) problems.append(q(p));
    executor.insert(QStringLiteral("problems"), problems);
    root.insert(QStringLiteral("executor"), executor);
  }
  if (sources.hub != nullptr) {
    QJsonObject log;
    log.insert(QStringLiteral("dock_level"), q(std::string(to_string(sources.hub->dock_level()))));
    log.insert(QStringLiteral("file"), from_path(sources.hub->file_path()));
    root.insert(QStringLiteral("log"), log);
  }
  return QJsonDocument(root).toJson(QJsonDocument::Indented).toStdString();
}

// The worker's result, waiting for the main thread. `target` is cleared by
// the destructor under `mutex`, so a worker finishing then signals nobody.
struct DiagnosticsCollector::Gate {
  std::mutex mutex;
  DiagnosticsCollector* target = nullptr;
  std::optional<Result<std::filesystem::path>> result;
};

DiagnosticsCollector::DiagnosticsCollector(QWidget* window, DiagnosticsDeps deps)
    : QObject(window), window_(window), deps_(std::move(deps)), gate_(std::make_shared<Gate>()) {
  setObjectName(QStringLiteral("diagnosticsCollector"));
  gate_->target = this;
  connect(this, &DiagnosticsCollector::resultQueued, this, &DiagnosticsCollector::deliver, Qt::QueuedConnection);
}

DiagnosticsCollector::~DiagnosticsCollector() {
  {
    const std::lock_guard lock(gate_->mutex);
    gate_->target = nullptr;
  }
  // The worker never waits on anything but the file system: it ends.
  if (worker_.joinable()) worker_.join();
}

void DiagnosticsCollector::collect() {
  if (busy()) {
    if (progress_) progress_->raise();
    return;
  }
  setup::BundleOptions options;
  options.out = deps_.out;
  options.log_dir = deps_.log_dir;
  options.trace_dir = deps_.trace_dir;
  options.site_path = deps_.site_path;
  options.hub = deps_.hub;
  options.clock = deps_.clock;
  options.qt_version = qVersion();
  // Taken here, on the main thread; the worker gets a copy of the text.
  if (deps_.snapshot) {
    options.live = [text = deps_.snapshot()] { return text; };
  }

  progress_ = new QProgressDialog(tr("Collecting diagnostics…"), QString(), 0, 0, window_);
  progress_->setObjectName(QStringLiteral("collectDiagnosticsProgress"));
  progress_->setWindowTitle(tr("Collect Diagnostics"));
  progress_->setCancelButton(nullptr);
  progress_->setWindowModality(Qt::NonModal);  // an instrument may be running: never blocks
  progress_->setMinimumDuration(0);
  progress_->show();

  auto write = deps_.write;
  if (!write) write = setup::write_support_bundle;
  worker_ = std::thread([gate = gate_, install = deps_.install, options = std::move(options), write = std::move(write)] {
    // An exception leaving this thread would end the program, and with it a
    // running instrument: it is a failed write like any other.
    std::optional<Result<std::filesystem::path>> written;
    try {
      written = write(install ? &*install : nullptr, options);
    } catch (const std::exception& e) {
      written = fail(ErrorKind::Io, std::string("support bundle not written: ") + e.what());
    } catch (...) {
      written = fail(ErrorKind::Io, "support bundle not written: unknown error");
    }
    const std::lock_guard lock(gate->mutex);
    gate->result = std::move(written);
    if (gate->target != nullptr) emit gate->target->resultQueued();
  });
}

void DiagnosticsCollector::deliver() {
  std::optional<Result<std::filesystem::path>> result;
  {
    const std::lock_guard lock(gate_->mutex);
    result = std::move(gate_->result);
    gate_->result.reset();
  }
  if (!result) return;
  // It has handed its result over and is leaving.
  if (worker_.joinable()) worker_.join();
  if (progress_) {
    progress_->close();
    progress_->deleteLater();
  }
  const QString path = *result ? from_path(**result) : QString();
  const QString error = *result ? QString() : q(result->error().what);
  show_result(path, error);
  emit finished(path, error);
}

void DiagnosticsCollector::show_result(const QString& path, const QString& error) {
  auto* box = new QMessageBox(window_);
  box->setObjectName(QStringLiteral("collectDiagnosticsResult"));
  box->setAttribute(Qt::WA_DeleteOnClose);
  box->setWindowModality(Qt::NonModal);
  box->setWindowTitle(tr("Collect Diagnostics"));
  if (error.isEmpty()) {
    const QString native = QDir::toNativeSeparators(path);
    box->setIcon(QMessageBox::Information);
    box->setText(tr("Diagnostics saved to %1").arg(native));
    box->setInformativeText(
        tr("Send this file to support by email or a file share. Passwords and keys in it are masked; "
           "credentials.toml is never included. Nothing was uploaded."));
    auto* reveal = box->addButton(reveal_label(), QMessageBox::ActionRole);
    reveal->setObjectName(QStringLiteral("revealBundleButton"));
    // An action-role button would close the box; keep it open and show the file.
    disconnect(reveal, &QAbstractButton::clicked, nullptr, nullptr);
    auto show = deps_.reveal;
    if (!show) show = reveal_in_file_manager;
    connect(reveal, &QAbstractButton::clicked, box, [path, show = std::move(show)] { show(path); });
    box->addButton(QMessageBox::Ok);
  } else {
    box->setIcon(QMessageBox::Warning);
    box->setText(tr("Diagnostics were not collected."));
    box->setInformativeText(error);
    box->addButton(QMessageBox::Ok);
  }
  box->show();
}

DiagnosticsCollector* diagnostics_collector(QWidget* window) {
  auto* found = window->findChild<DiagnosticsCollector*>(QString(), Qt::FindDirectChildrenOnly);
  if (found != nullptr) return found;
  DiagnosticsDeps deps;
  deps.hub = process_log_hub();
  return new DiagnosticsCollector(window, std::move(deps));
}

void reveal_in_file_manager(const QString& path) {
#if defined(Q_OS_MACOS)
  QProcess::startDetached(QStringLiteral("open"), {QStringLiteral("-R"), path});
#elif defined(Q_OS_WIN)
  QProcess::startDetached(QStringLiteral("explorer.exe"),
                          {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
  QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
}

}  // namespace nobion::ui
