#pragma once

// ValvePopup (spec 2026-11-04-hmi-valves §6): a valve's faceplate, what a
// left click on it shows beside it. What the valve is, its state and since
// when, the interlocks its opening waits on (met or not), its history, the
// last rejection, and the controls: Open / Close and Lock / Unlock. A
// read-only canvas or a manual valve gets no controls.
//
// A Qt::Popup window: it takes the focus, and Esc or a click outside closes
// it. It holds no state of the line: CanvasView pushes a ValveFaceplate
// (set_faceplate) when it opens and whenever the state changes, and a
// control calls back to the view, which sends the command.

#include <functional>
#include <string>
#include <vector>

#include <QFrame>
#include <QRect>
#include <QString>
#include <QStringList>

#include "nobion/devices/types.hpp"
#include "nobion/systems/canvas/canvas.hpp"
#include "nobion/systems/switch_manager.hpp"

class QLabel;
class QPushButton;

namespace nobion::ui {

// What the faceplate shows of one valve.
struct ValveFaceplate {
  struct Interlock {
    QString partner;
    bool must_be_closed = true;  // an exclusive partner; false: a positive one, which must be open
    bool met = false;
  };
  std::string name;
  QString title;        // the label, or the name when it has none
  QString description;  // empty when the line gives none
  canvas::ValveKind kind = canvas::ValveKind::Valve;
  ValveState state = ValveState::Unknown;
  QString state_line;  // "Open since 13:41:07 (22 min)", locked, owner
  std::vector<Interlock> interlocks;
  QStringList history;  // last actuated, counts, time open
  QString last_failure;
  bool locked = false;
  bool pending = false;
  bool operable = false;  // the controls are shown: not read-only, not manual
};

class ValvePopup : public QFrame {
  Q_OBJECT

 public:
  explicit ValvePopup(QWidget* parent);

  void set_faceplate(const ValveFaceplate& faceplate);
  [[nodiscard]] const ValveFaceplate& faceplate() const noexcept { return faceplate_; }
  // Beside `global_valve` (the valve's body on the screen), clear of it,
  // inside the screen.
  void show_beside(const QRect& global_valve);

  void set_on_operate(std::function<void(const std::string&, systems::SwitchOp)> cb) { on_operate_ = std::move(cb); }
  void set_on_lock(std::function<void(const std::string&, bool)> cb) { on_lock_ = std::move(cb); }

  [[nodiscard]] QPushButton* open_button() const noexcept { return open_; }
  [[nodiscard]] QPushButton* close_button() const noexcept { return close_; }
  [[nodiscard]] QPushButton* lock_button() const noexcept { return lock_; }
  // Every line of text shown, top to bottom, for tests.
  [[nodiscard]] QStringList text() const;

 signals:
  // Hidden: Esc, a click outside, or the view closing it.
  void closed();

 protected:
  void hideEvent(QHideEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

 private:
  ValveFaceplate faceplate_;
  QLabel* title_;
  QLabel* description_;
  QLabel* state_;
  QLabel* interlocks_;
  QLabel* history_;
  QLabel* failure_;
  QLabel* note_;
  QPushButton* open_;
  QPushButton* close_;
  QPushButton* lock_;
  std::function<void(const std::string&, systems::SwitchOp)> on_operate_;
  std::function<void(const std::string&, bool)> on_lock_;
};

}  // namespace nobion::ui
