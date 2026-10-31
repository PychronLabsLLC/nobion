#include "grains_window.hpp"

#include <algorithm>
#include <utility>

#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "grain_overlay.hpp"
#include "pychron/grains/score.hpp"

namespace pychron::grains {

namespace {

constexpr const char* kSideKey = "reference/side";
constexpr const char* kFolderKey = "recording/folder";

QString vote_text(Vote vote) {
  switch (vote) {
    case Vote::Same: return QObject::tr("moved the same way as the reference");
    case Vote::Opposite: return QObject::tr("moved the opposite way to the reference");
    case Vote::None: break;
  }
  return QObject::tr("no result");
}

}  // namespace

QString grain_detail_text(const GrainView& grain, const GrainOptions& options) {
  const std::string_view label = label_text(grain.label);
  QStringList lines;
  lines << QObject::tr("grain %1%2").arg(grain.id).arg(grain.reference ? QObject::tr(" (reference)") : QString());
  if (!label.empty()) {
    lines << QObject::tr("label: %1, score %2")
                 .arg(QString::fromUtf8(label.data(), static_cast<qsizetype>(label.size())))
                 .arg(grain.score, 0, 'f', 2);
  } else {
    lines << QObject::tr("label: none yet");
  }
  lines << QObject::tr("passes: %1 for this label, %2 against it, %3 without a result")
               .arg(grain.passes_agree)
               .arg(grain.passes_against)
               .arg(grain.passes_none);
  lines << QObject::tr("last pass: %1").arg(vote_text(grain.last_vote));
  lines << QObject::tr("contrast: %1 of depth").arg(grain.contrast, 0, 'f', 3);
  if (grain.at_edge) lines << QObject::tr("cut by the frame edge");
  if (grain.label == Label::Unsure) {
    QString why;
    if (grain.passes_agree + grain.passes_against < options.min_passes) {
      why = QObject::tr("only %1 passes counted, %2 needed").arg(grain.passes_agree + grain.passes_against).arg(options.min_passes);
    } else if (grain.contrast < options.min_contrast) {
      why = QObject::tr("contrast under %1").arg(options.min_contrast, 0, 'f', 3);
    } else {
      why = QObject::tr("score %1 under %2 either way").arg(grain.score, 0, 'f', 2).arg(options.min_score, 0, 'f', 2);
    }
    lines << QObject::tr("unsure because: %1").arg(why);
  }
  return lines.join(QLatin1Char('\n'));
}

GrainsWindow::GrainsWindow(std::function<FrameTick()> poll, GrainOptions options, QWidget* parent)
    : QMainWindow(parent), options_(options) {
  setWindowTitle(tr("grains"));

  overlay_ = new GrainOverlay(this);
  auto* panel = new QWidget(this);
  panel->setFixedWidth(280);
  auto* panel_layout = new QVBoxLayout(panel);
  panel_layout->addWidget(new QLabel(tr("Grain"), panel));
  detail_ = new QPlainTextEdit(panel);
  detail_->setObjectName(QStringLiteral("detail"));
  detail_->setReadOnly(true);
  detail_->setPlainText(tr("click a grain for its pass history"));
  panel_layout->addWidget(detail_, 1);
  pin_ = new QPushButton(tr("Mark picked"), panel);
  pin_->setObjectName(QStringLiteral("pin"));
  pin_->setEnabled(false);
  panel_layout->addWidget(pin_);

  auto* centre = new QWidget(this);
  auto* row = new QHBoxLayout(centre);
  row->setContentsMargins(0, 0, 0, 0);
  row->addWidget(overlay_, 1);
  row->addWidget(panel);
  setCentralWidget(centre);

  auto* bar = addToolBar(tr("Tools"));
  bar->setMovable(false);
  reference_tool_ = new QToolButton(bar);
  reference_tool_->setObjectName(QStringLiteral("reference_tool"));
  reference_tool_->setText(tr("Reference tool"));
  reference_tool_->setCheckable(true);
  reference_tool_->setToolTip(tr("Click a known grain to make it the reference"));
  bar->addWidget(reference_tool_);
  side_ = new QComboBox(bar);
  side_->setObjectName(QStringLiteral("side"));
  side_->addItem(tr("below oil"), static_cast<int>(Side::Below));
  side_->addItem(tr("above oil"), static_cast<int>(Side::Above));
  side_->setToolTip(tr("Which side of the oil the reference grain is on"));
  {
    const QSettings settings;
    const int index = side_->findData(settings.value(QLatin1String(kSideKey), static_cast<int>(Side::Below)).toInt());
    side_->setCurrentIndex(index < 0 ? 0 : index);
    record_folder_ = settings.value(QLatin1String(kFolderKey)).toString();
  }
  bar->addWidget(side_);
  second_tool_ = new QToolButton(bar);
  second_tool_->setObjectName(QStringLiteral("second_tool"));
  second_tool_->setText(tr("Second reference"));
  second_tool_->setCheckable(true);
  second_tool_->setToolTip(tr("Click a grain of the opposite side: the two must move opposite ways"));
  bar->addWidget(second_tool_);
  clear_ = new QPushButton(tr("Clear reference"), bar);
  clear_->setObjectName(QStringLiteral("clear_reference"));
  bar->addWidget(clear_);
  bar->addSeparator();
  record_ = new QPushButton(tr("Record"), bar);
  record_->setObjectName(QStringLiteral("record"));
  record_->setCheckable(true);
  bar->addWidget(record_);
  folder_ = new QPushButton(tr("Folder..."), bar);
  folder_->setObjectName(QStringLiteral("record_folder"));
  bar->addWidget(folder_);

  connect(reference_tool_, &QToolButton::toggled, this, [this](bool on) {
    if (on) second_tool_->setChecked(false);
  });
  connect(second_tool_, &QToolButton::toggled, this, [this](bool on) {
    if (on) reference_tool_->setChecked(false);
  });
  connect(side_, &QComboBox::currentIndexChanged, this, &GrainsWindow::side_changed);
  connect(clear_, &QPushButton::clicked, this, [this] {
    runner_->clear_reference();
    declared_side_.reset();
    reference_notice_.clear();
    apply_notice();
  });
  connect(record_, &QPushButton::toggled, this, &GrainsWindow::on_record_toggled);
  connect(folder_, &QPushButton::clicked, this, &GrainsWindow::choose_folder);
  connect(pin_, &QPushButton::clicked, this, &GrainsWindow::toggle_pin);
  connect(overlay_, &GrainOverlay::frame_clicked, this, &GrainsWindow::on_click);

  runner_ = std::make_unique<GrainRunner>(std::move(poll), options, [this] {
    // carries nothing: the slot takes the report
    if (!report_pending_.exchange(true)) QMetaObject::invokeMethod(this, &GrainsWindow::on_report_ready, Qt::QueuedConnection);
  });
}

GrainsWindow::~GrainsWindow() { runner_.reset(); }

Side GrainsWindow::chosen_side() const { return static_cast<Side>(side_->currentData().toInt()); }

void GrainsWindow::side_changed() {
  QSettings settings;
  settings.setValue(QLatin1String(kSideKey), side_->currentData().toInt());
}

void GrainsWindow::show_notice(const QString& notice) {
  hint_ = notice;
  apply_notice();
}

void GrainsWindow::apply_notice() {
  QStringList parts;
  for (const QString& part : {reference_notice_, recording_error_, hint_}) {
    if (!part.isEmpty()) parts << part;
  }
  overlay_->set_notice(parts.join(QStringLiteral("  |  ")));
}

void GrainsWindow::set_record_folder(const QString& folder) { record_folder_ = folder; }

void GrainsWindow::on_report_ready() {
  report_pending_ = false;
  std::optional<GrainRunner::Shown> shown = runner_->take_report();
  if (!shown) return;
  if (!shown->error.empty()) {
    overlay_->set_failed(QString::fromStdString(shown->error));
  } else if (shown->frame) {
    // A reset restarts the grain ids: what was picked or selected would sit on
    // other grains.
    if (last_generation_ && *last_generation_ != shown->report.generation) {
      overlay_->clear_picked();
      selected_.reset();
      overlay_->set_selected(selected_);
      if (shown->report.reference == ReferenceState::None) declared_side_.reset();
    }
    last_generation_ = shown->report.generation;
    overlay_->show_frame(*shown->frame, shown->report);
  }
  // A report made before stop_recording must not wipe what stop said.
  if (recording_) {
    const QString recording_error = QString::fromStdString(shown->recording_error);
    if (recording_error != recording_error_) {
      recording_error_ = recording_error;
      apply_notice();
    }
  }
  refresh_detail();
}

void GrainsWindow::post_done(Done done) {
  {
    const std::scoped_lock lock(done_mutex_);
    done_.push_back(std::move(done));
  }
  QMetaObject::invokeMethod(this, &GrainsWindow::on_done_ready, Qt::QueuedConnection);
}

void GrainsWindow::on_done_ready() {
  std::vector<Done> results;
  {
    const std::scoped_lock lock(done_mutex_);
    results.swap(done_);
  }
  for (const Done& done : results) {
    if (done.ok) {
      reference_notice_.clear();
      if (!done.second) {
        declared_side_ = done.side;
        overlay_->set_side(done.side);
      }
    } else {
      reference_notice_ = QString::fromStdString(done.what);
    }
  }
  apply_notice();
}

void GrainsWindow::on_click(QPointF frame_px) {
  const bool first = reference_tool_->isChecked();
  const bool second = second_tool_->isChecked();
  if (first || second) {
    // The side of the second is the opposite of the declared first one.
    const Side first_side = declared_side_.value_or(chosen_side());
    const Side side = first ? chosen_side() : (first_side == Side::Below ? Side::Above : Side::Below);
    reference_tool_->setChecked(false);
    second_tool_->setChecked(false);
    const vision::Vec2 at{frame_px.x(), frame_px.y()};
    auto done = [this, second, side](Result<void> result) {
      post_done({second, side, result.has_value(), result ? std::string() : result.error().what});
    };
    if (first) {
      runner_->set_reference(at, side, std::move(done));
    } else {
      runner_->set_second_reference(at, side, std::move(done));
    }
    return;
  }
  selected_ = overlay_->grain_at(frame_px);
  overlay_->set_selected(selected_);
  refresh_detail();
}

void GrainsWindow::refresh_detail() {
  if (!selected_) {
    pin_->setEnabled(false);
    return;
  }
  pin_->setEnabled(true);
  pin_->setText(overlay_->is_picked(*selected_) ? tr("Unmark picked") : tr("Mark picked"));
  const auto& grains = overlay_->report().grains;
  const auto it = std::ranges::find_if(grains, [&](const GrainView& g) { return g.id == *selected_; });
  const QString text = it == grains.end() ? tr("grain %1: not in view").arg(*selected_) : grain_detail_text(*it, options_);
  if (detail_->toPlainText() != text) detail_->setPlainText(text);
}

void GrainsWindow::toggle_pin() {
  if (!selected_) return;
  overlay_->set_picked(*selected_, !overlay_->is_picked(*selected_));
  refresh_detail();
}

void GrainsWindow::choose_folder() {
  const QString folder = QFileDialog::getExistingDirectory(this, tr("Folder for recordings"), record_folder_);
  if (folder.isEmpty()) return;
  record_folder_ = folder;
  QSettings settings;
  settings.setValue(QLatin1String(kFolderKey), folder);
}

void GrainsWindow::set_recording_button(bool on) {
  const QSignalBlocker blocker(record_);
  record_->setChecked(on);
  record_->setText(on ? tr("Stop recording") : tr("Record"));
}

void GrainsWindow::on_record_toggled(bool on) {
  if (on) {
    if (record_folder_.isEmpty()) choose_folder();
    if (record_folder_.isEmpty()) {
      set_recording_button(false);
      return;
    }
    const QString path = QDir(record_folder_).filePath(QStringLiteral("grains-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    const Result<void> started = runner_->start_recording(path.toStdString());
    if (!started) {
      recording_error_ = QString::fromStdString(started.error().what);
      apply_notice();
      set_recording_button(false);
      return;
    }
    recording_path_ = path;
    recording_ = true;
    recording_error_.clear();
    // Grain ids start again from 1, so what was picked or selected means nothing now.
    overlay_->clear_picked();
    selected_.reset();
    overlay_->set_selected(selected_);
    set_recording_button(true);
    apply_notice();
    return;
  }
  if (!recording_) {
    set_recording_button(false);
    return;
  }
  recording_ = false;
  const Result<void> stopped = runner_->stop_recording();
  recording_error_ = stopped ? QString() : QString::fromStdString(stopped.error().what);
  set_recording_button(false);
  apply_notice();
}

}  // namespace pychron::grains
