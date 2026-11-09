#pragma once

// Help > Collect Diagnostics… and the unclean-exit dialog's Collect button
// (field diagnostics spec section 8): one support bundle
// (setup::write_support_bundle) written from the running program.
//
// The live state (live.json) is taken on the main thread when collect() is
// called, as a JSON string; a worker thread then only writes the zip. The
// result comes back the way SpectrometerBridge's do: under the collector's
// own mutex, with a queued signal that carries nothing (Qt is not built with
// ThreadSanitizer, so nothing crosses in a queued functor). While it runs a
// progress dialog is up; then a message says where the zip is and offers to
// show it in the file manager, or says why it was not written. Nothing is
// uploaded.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <QObject>
#include <QPointer>
#include <QString>

#include "nobion/core/error.hpp"
#include "nobion/setup/site.hpp"

class QProgressDialog;
class QWidget;

namespace nobion {
class Clock;
class LogHub;
}  // namespace nobion

namespace nobion::setup {
struct BundleOptions;
}  // namespace nobion::setup

namespace nobion::ui {

class AlarmDock;
class CoreBridge;
class ExperimentBridge;

// What a live snapshot is read from; any may be null.
struct LiveSources {
  const CoreBridge* core = nullptr;          // transports, pressures, pending actuations
  const AlarmDock* alarms = nullptr;         // standing alarms
  ExperimentBridge* experiment = nullptr;    // executor state, saves still to make
  const LogHub* hub = nullptr;               // the Log dock's level
};

// The running program's state as JSON, read on the main thread. Always has a
// `transports` object (empty without a line).
std::string live_state_json(const LiveSources& sources);

struct DiagnosticsDeps {
  // The install whose config, state and doctor report go in; none: left out.
  std::optional<setup::SiteInstall> install;
  // Called on the main thread at each collect(): live.json's text. Empty: no live.json.
  std::function<std::string()> snapshot;
  // Stamps in the bundle; must outlive the collector. Null: system time.
  const Clock* clock = nullptr;
  // The process's hub: a fresh flight recorder dump and the log folder.
  std::shared_ptr<LogHub> hub;
  // As BundleOptions; empty: its defaults. Tests give their own.
  std::filesystem::path out, log_dir, trace_dir, site_path;
  // Writes the bundle, on the worker; empty: setup::write_support_bundle.
  // Tests give one of their own (one that throws, one that waits).
  std::function<Result<std::filesystem::path>(const setup::SiteInstall*, const setup::BundleOptions&)> write;
  // The result message's reveal button; empty: reveal_in_file_manager.
  std::function<void(const QString&)> reveal;
};

class DiagnosticsCollector : public QObject {
  Q_OBJECT

 public:
  // Parented to `window`, over which its dialogs are shown. Help > Collect
  // Diagnostics… of that window (brand::add_help_menu) uses it.
  DiagnosticsCollector(QWidget* window, DiagnosticsDeps deps);
  // Waits for a bundle still being written.
  ~DiagnosticsCollector() override;
  DiagnosticsCollector(const DiagnosticsCollector&) = delete;
  DiagnosticsCollector& operator=(const DiagnosticsCollector&) = delete;
  DiagnosticsCollector(DiagnosticsCollector&&) = delete;
  DiagnosticsCollector& operator=(DiagnosticsCollector&&) = delete;

  // Main thread. Takes the snapshot and starts the write; while one is being
  // written, only brings its progress dialog forward.
  void collect();
  bool busy() const noexcept { return worker_.joinable(); }

 signals:
  // On the main thread once the write ended: the zip's path, or the error.
  void finished(const QString& path, const QString& error);
  // Worker to main thread: a result is waiting in the gate. Carries nothing.
  void resultQueued();

 private:
  struct Gate;

  void deliver();
  void show_result(const QString& path, const QString& error);

  QWidget* window_;
  DiagnosticsDeps deps_;
  std::shared_ptr<Gate> gate_;
  std::thread worker_;
  QPointer<QProgressDialog> progress_;
};

// The collector of `window`: the one made for it, else one made now with no
// install, no live state and the process log hub.
DiagnosticsCollector* diagnostics_collector(QWidget* window);

// Shows `path` in Finder, Explorer, or (elsewhere) opens its folder.
void reveal_in_file_manager(const QString& path);

}  // namespace nobion::ui
