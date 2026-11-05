#pragma once

// StatusBanner and AlarmStrip (spec 2026-11-05-hmi-banner): the band across
// the top of the main window and the alarms under it. Both hold no state of
// the line: the main window pushes what they show, and they count what the
// Alarms dock and the health bar already decided (one rule each), never
// working it out again.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <QDateTime>
#include <QFrame>
#include <QString>
#include <QTimer>

#include "alarm_dock.hpp"
#include "health_bar.hpp"
#include "pychron/experiment/executor/executor.hpp"

class QLabel;
class QToolButton;
class QVBoxLayout;

namespace pychron::ui {

// Whether the line is real hardware or simulated, and how fast a simulated
// one runs.
struct LineMode {
  bool simulated = false;
  double speed = 0;  // 0: real time; > 0: a VirtualClock at N x
};

class StatusBanner : public QFrame {
  Q_OBJECT

 public:
  static constexpr int kHeight = 30;
  static constexpr int kClockPadding = 8;  // every field's padding, each side
  static constexpr int kMinField = 40;     // the narrowest the name and executor fields get

  // `clock` is the line's wall clock (simulated time in a simulation).
  explicit StatusBanner(std::function<QDateTime()> clock, QWidget* parent = nullptr);

  // Also says, in the clock's tooltip, which clock it reads.
  void set_mode(LineMode mode);
  void set_line(const QString& name);

  // Executor: no session until set_session(true); then each event moves
  // what the field says. `running`: a queue already runs (its run is named
  // when the next starts).
  void set_session(bool has_session, bool running = false);
  void on_state(const experiment::executor::ExecutorStateChanged& e);
  void on_run_started(const experiment::executor::RunStarted& e);
  void on_holding(const experiment::executor::ExecutorHolding& e);
  // The text for a state, the run it is on and why it holds.
  static QString executor_text(std::optional<experiment::executor::ExecutorState> state, const QString& run,
                               const QString& hold_reason);

  // Counts Critical and Warning rows; Info is the dock's alone.
  void set_alarms(const std::vector<AlarmRow>& rows);
  void set_health(const HealthBar::Summary& summary);
  // Reads the clock now (a 1 s timer also does).
  void tick();

  [[nodiscard]] QString mode_text() const;
  [[nodiscard]] QString line_text() const;
  [[nodiscard]] QString executor_text() const;
  [[nodiscard]] QString alarms_text() const;
  [[nodiscard]] QString health_text() const;
  [[nodiscard]] QString clock_text() const;
  [[nodiscard]] QToolButton* alarms_button() const noexcept { return alarms_; }

 signals:
  // The alarm counts were clicked: the window shows its Alarms dock.
  void alarms_clicked();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  void render_executor();
  void fit_clock();

  std::function<QDateTime()> clock_;
  QLabel* mode_;
  QLabel* line_;
  QLabel* executor_;
  QToolButton* alarms_;
  QLabel* health_;
  QLabel* clock_label_;
  QTimer ticker_;
  bool session_ = false;
  std::optional<experiment::executor::ExecutorState> state_;
  QString run_;
  QString hold_reason_;
};

// The standing Critical and Warning alarms, most pressing first, under the
// banner; hidden while there are none. A click anywhere asks for the dock,
// where they are acknowledged.
class AlarmStrip : public QFrame {
  Q_OBJECT

 public:
  static constexpr int kShown = 3;

  explicit AlarmStrip(QWidget* parent = nullptr);

  // `rows` as AlarmDock::rows() gives them (most pressing first).
  void set_alarms(const std::vector<AlarmRow>& rows);
  // One line per row shown, then "+N more" when there are more.
  [[nodiscard]] const QStringList& lines() const noexcept { return lines_; }

 signals:
  void clicked();

 protected:
  void mousePressEvent(QMouseEvent* event) override;

 private:
  QVBoxLayout* layout_;
  QStringList lines_;
};

}  // namespace pychron::ui
