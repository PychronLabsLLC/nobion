#pragma once

// HealthBar: one chip per transport, green (connected, no errors), amber
// (connected with errors, or with a device Degraded or Faulted), red (down),
// grey (no report yet), plus the age of the last healthy report, refreshed
// once a second. The tooltip lists the transport's devices in name order.
// Neither chips nor their device entries are ever removed. While the queue
// holds (spec 4.6), a chip ahead of them says why and counts down to the hold
// limit, at which the queue ends: "holding: <reason> · ends in 1:59:30".

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QHBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QWidget>

#include "nobion/core/events.hpp"

#include "theme.hpp"

namespace nobion::ui {

class HealthBar : public QWidget {
  Q_OBJECT

 public:
  enum class Status { Unknown, Ok, Degraded, Down };
  // How many transports are in each status.
  struct Summary {
    int ok = 0;
    int degraded = 0;
    int down = 0;
    int unknown = 0;
    friend bool operator==(const Summary&, const Summary&) = default;
  };

  // `now` must use the core Clock's epoch; defaults to steady_clock.
  explicit HealthBar(QWidget* parent = nullptr, std::function<TimePoint()> now = {});

  // Adds a grey chip for each transport not shown yet.
  void seed(const std::vector<std::string>& transports);
  void update_health(const TransportHealth& health);
  // A device on a transport changed state: the chip turns amber while one is
  // Degraded or Faulted (a down transport stays red), and its tooltip lists
  // every device seen on it.
  void update_device(const DeviceHealthChanged& event);
  // Re-renders every chip's age text and the hold's countdown.
  void refresh();

  // The queue holds for `reason` until `until` (ExecutorHolding::until, on
  // the core Clock, as `now`); clear_hold() when it no longer does.
  void show_hold(const QString& reason, TimePoint until);
  void clear_hold();
  bool holding() const noexcept { return hold_.has_value(); }
  // The hold chip's text at `now`; empty without a hold.
  QString hold_text_at(TimePoint now) const;
  // As shown (empty without a hold).
  QString hold_text() const;

  static Status status_of(const TransportHealth& health);
  static style::Level level_of(Status status);

  std::optional<Status> status(const std::string& transport) const;
  // As shown on the chip; Unknown for a transport not on the bar.
  Status chip_status(const QString& transport) const;
  QLabel* chip(const std::string& transport) const;
  [[nodiscard]] Summary summary() const;

 signals:
  // A transport was added or its status changed (not on the age ticks).
  void changed();

 private:
  struct Chip {
    QLabel* label = nullptr;
    Status status = Status::Unknown;  // the transport's own report
    std::optional<TimePoint> last_ok;
    QString transport_tip;
    struct Device {
      DeviceState state = DeviceState::Unknown;
      std::uint64_t failures = 0;
      std::string error;
      std::optional<TimePoint> last_ok;
    };
    std::map<std::string, Device> devices;
  };
  static Status shown_status(const Chip& chip);

  struct Hold {
    QString reason;
    TimePoint until;
  };

  Chip& chip_for(const std::string& transport);
  void render(const std::string& name, const Chip& chip);
  void render_hold();

  std::function<TimePoint()> now_;
  QHBoxLayout* layout_;
  QLabel* hold_chip_;
  std::optional<Hold> hold_;
  QTimer ticker_;
  std::map<std::string, Chip> chips_;
};

}  // namespace nobion::ui
