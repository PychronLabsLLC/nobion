#pragma once

// AlarmDock: active alarms, one row per source (a repeat alarm updates the
// row). Acknowledging removes rows locally; nothing is sent to the core (M1).

#include <functional>
#include <string>
#include <vector>

#include <QDateTime>
#include <QDockWidget>
#include <QPushButton>
#include <QTreeWidget>

#include "nobion/core/events.hpp"

namespace pychron::ui {

// A standing alarm: a row of the dock.
struct AlarmRow {
  std::string source;
  AlarmSeverity severity = AlarmSeverity::Warning;
  std::string message;
  QDateTime time;  // when the row was last raised, on the dock's clock
};

class AlarmDock : public QDockWidget {
  Q_OBJECT

 public:
  // `now` is the time a row is stamped with; the real time when empty. The
  // main window gives the line's clock, which is not real time on a
  // simulated line.
  explicit AlarmDock(QWidget* parent = nullptr, std::function<QDateTime()> now = {});

  void add_alarm(const Alarm& alarm);
  // Removes the selected rows; returns how many were acknowledged.
  int acknowledge_selected();
  void acknowledge_all();

  int active_count() const;
  bool is_active(const std::string& source) const;
  QTreeWidget* tree() const noexcept { return tree_; }
  // The standing alarms, most pressing first: Critical, then Warning, then
  // Info, and newest first within each (spec 2026-11-05-hmi-banner §5).
  std::vector<AlarmRow> rows() const;

 signals:
  // After an alarm was added or changed (severity or message), or rows were
  // acknowledged; not for a repeat of what a row already says.
  void changed();

 private:
  QTreeWidgetItem* row_for(const QString& source) const;

  std::function<QDateTime()> now_;
  quint64 raised_ = 0;  // counts raises: the order rows() breaks ties by
  QTreeWidget* tree_;
  QPushButton* ack_;
  QPushButton* ack_all_;
};

}  // namespace pychron::ui
