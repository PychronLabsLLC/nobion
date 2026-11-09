#pragma once

// "<program> did not close cleanly last time" (spec
// 2026-11-06-field-diagnostics, 3.3): shown once the main window is up when
// the log folder held the sentinel of a session that is gone without a clean
// end. The program is the sentinel's (nobion or elctl).

#include <QDialog>

#include "nobion/core/log_hub.hpp"

class QWidget;

namespace nobion::ui {

class UncleanExitDialog : public QDialog {
  Q_OBJECT

 public:
  UncleanExitDialog(const PreviousSession& previous, QWidget* parent);

 signals:
  // The operator asked for diagnostics. The dialog closes after emitting it.
  void collectRequested();
};

// Shows the dialog over `window` when the process log hub found an unclean
// previous session; nothing otherwise. Not modal and never blocking: an
// instrument may be running behind it, and the operator must be able to keep
// working. It deletes itself on close. Returns the dialog (null when none is
// shown), whose collectRequested the caller connects to the window's
// DiagnosticsCollector (collect_diagnostics.hpp).
UncleanExitDialog* show_unclean_exit_notice(QWidget* window);

}  // namespace nobion::ui
