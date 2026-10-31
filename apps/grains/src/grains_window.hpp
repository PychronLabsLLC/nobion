#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <QMainWindow>
#include <QPointF>
#include <QString>

#include "pychron/grains/pipeline.hpp"
#include "pychron/grains/runner.hpp"
#include "pychron/grains/types.hpp"

class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace pychron::grains {

class GrainOverlay;

// What the detail panel says of one grain: its pass history, contrast and, for
// an unsure one, why.
[[nodiscard]] QString grain_detail_text(const GrainView& grain, const GrainOptions& options);

// The window: the overlay, a toolbar (reference tool and its side, a second
// reference, recording) and the detail panel. The worker is a GrainRunner whose
// callbacks carry nothing to this thread: the window takes the newest report
// when woken, and `done` results through a mailbox of its own.
class GrainsWindow : public QMainWindow {
  Q_OBJECT
 public:
  GrainsWindow(std::function<FrameTick()> poll, GrainOptions options, QWidget* parent = nullptr);
  // The runner goes first, so no callback reaches a window that is going.
  ~GrainsWindow() override;

  [[nodiscard]] GrainOverlay* overlay() const { return overlay_; }
  // A line for the banner until the next reference result replaces it.
  void show_notice(const QString& notice);
  // The folder recordings go under; empty: asked at the first recording.
  void set_record_folder(const QString& folder);
  // The folder of the recording open or last made.
  [[nodiscard]] const QString& recording_path() const { return recording_path_; }
  [[nodiscard]] std::optional<std::uint64_t> selected() const { return selected_; }

 private slots:
  void on_report_ready();
  void on_done_ready();
  void on_click(QPointF frame_px);
  void on_record_toggled(bool on);
  void choose_folder();
  void toggle_pin();
  void side_changed();

 private:
  struct Done {
    bool second = false;
    Side side = Side::Below;
    bool ok = true;
    std::string what;
  };

  [[nodiscard]] Side chosen_side() const;
  void post_done(Done done);
  void refresh_detail();
  void apply_notice();
  void set_recording_button(bool on);

  GrainOptions options_;
  GrainOverlay* overlay_ = nullptr;
  QToolButton* reference_tool_ = nullptr;
  QToolButton* second_tool_ = nullptr;
  QComboBox* side_ = nullptr;
  QPushButton* clear_ = nullptr;
  QPushButton* record_ = nullptr;
  QPushButton* folder_ = nullptr;
  QPlainTextEdit* detail_ = nullptr;
  QPushButton* pin_ = nullptr;

  std::optional<std::uint64_t> selected_;
  std::optional<Side> declared_side_;
  QString reference_notice_;
  QString recording_error_;
  QString hint_;
  QString record_folder_;
  QString recording_path_;
  bool recording_ = false;

  std::atomic<bool> report_pending_{false};
  std::mutex done_mutex_;
  std::vector<Done> done_;  // guarded by done_mutex_

  std::unique_ptr<GrainRunner> runner_;  // last: destroyed first
};

}  // namespace pychron::grains
