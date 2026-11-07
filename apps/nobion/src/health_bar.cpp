#include "health_bar.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace nobion::ui {

namespace {

QString age_text(std::chrono::seconds age) {
  const auto s = age.count();
  if (s < 60) {
    return QStringLiteral("%1s").arg(s);
  }
  if (s < 3600) {
    return QStringLiteral("%1m").arg(s / 60);
  }
  return QStringLiteral("%1h").arg(s / 3600);
}

QString countdown_text(std::chrono::seconds left) {
  const auto s = std::max<long long>(left.count(), 0);
  return QStringLiteral("%1:%2:%3")
      .arg(s / 3600)
      .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
      .arg(s % 60, 2, 10, QLatin1Char('0'));
}

}  // namespace

HealthBar::HealthBar(QWidget* parent, std::function<TimePoint()> now)
    : QWidget(parent), now_(std::move(now)), layout_(new QHBoxLayout(this)), hold_chip_(new QLabel(this)) {
  if (!now_) {
    now_ = [] { return std::chrono::steady_clock::now(); };
  }
  layout_->setContentsMargins(4, 0, 4, 0);
  layout_->setSpacing(6);
  hold_chip_->setObjectName(QStringLiteral("HoldChip"));
  hold_chip_->setMargin(2);
  hold_chip_->setToolTip(tr("The queue holds: it starts the next run once the links it needs are back, "
                            "and ends when the count reaches zero (Experiment > Executor: Resume, Skip Held Run)"));
  hold_chip_->hide();
  layout_->addWidget(hold_chip_);
  ticker_.setInterval(1000);
  connect(&ticker_, &QTimer::timeout, this, &HealthBar::refresh);
  ticker_.start();
}

void HealthBar::seed(const std::vector<std::string>& transports) {
  const Summary before = summary();
  for (const auto& name : transports) {
    render(name, chip_for(name));
  }
  if (summary() != before) emit changed();
}

void HealthBar::update_health(const TransportHealth& health) {
  const Summary before = summary();
  Chip& chip = chip_for(health.transport);
  chip.status = status_of(health);
  if (health.connected) {
    chip.last_ok = health.ts;
  }
  QString tip = QStringLiteral("%1: %2 error(s)").arg(QString::fromStdString(health.transport)).arg(health.error_count);
  if (!health.last_error.empty()) {
    tip += QStringLiteral("\nlast error: ") + QString::fromStdString(health.last_error);
  }
  chip.transport_tip = std::move(tip);
  render(health.transport, chip);
  if (summary() != before) emit changed();
}

void HealthBar::update_device(const DeviceHealthChanged& event) {
  // A device made without a transport name has no chip to go on. One that only
  // turns Ok on a transport nothing else has reported on would add a grey chip
  // counted as unknown until the transport's own first health: wait for that.
  if (event.transport.empty()) return;
  if (event.to == DeviceState::Ok && !chips_.contains(event.transport)) return;
  const Summary before = summary();
  Chip& chip = chip_for(event.transport);
  Chip::Device& device = chip.devices[event.device];
  device.state = event.to;
  device.failures = event.consecutive_failures;
  device.error = event.last_error_text;
  // On a change from Ok the event's ts is the first failure, not the last
  // success: an approximation within one poll period of the device.
  if (event.to == DeviceState::Ok || event.from == DeviceState::Ok) {
    device.last_ok = event.ts;
  }
  render(event.transport, chip);
  if (summary() != before) emit changed();
}

HealthBar::Status HealthBar::shown_status(const Chip& chip) {
  if (chip.status == Status::Down || chip.status == Status::Degraded) {
    return chip.status;
  }
  const bool bad = std::ranges::any_of(chip.devices, [](const auto& d) {
    return d.second.state == DeviceState::Degraded || d.second.state == DeviceState::Faulted;
  });
  return bad ? Status::Degraded : chip.status;
}

HealthBar::Status HealthBar::chip_status(const QString& transport) const {
  const auto it = chips_.find(transport.toStdString());
  return it == chips_.end() ? Status::Unknown : shown_status(it->second);
}

HealthBar::Summary HealthBar::summary() const {
  Summary s;
  for (const auto& [name, chip] : chips_) {
    switch (shown_status(chip)) {
      case Status::Ok: ++s.ok; break;
      case Status::Degraded: ++s.degraded; break;
      case Status::Down: ++s.down; break;
      case Status::Unknown: ++s.unknown; break;
    }
  }
  return s;
}

void HealthBar::refresh() {
  for (const auto& [name, chip] : chips_) {
    render(name, chip);
  }
  render_hold();
}

void HealthBar::show_hold(const QString& reason, TimePoint until) {
  hold_ = Hold{reason, until};
  render_hold();
}

void HealthBar::clear_hold() {
  hold_.reset();
  render_hold();
}

QString HealthBar::hold_text_at(TimePoint now) const {
  if (!hold_) {
    return {};
  }
  // Rounded up: a hold with 0.4 s left still has time, and says so.
  const auto left = std::chrono::ceil<std::chrono::seconds>(hold_->until - now);
  return tr("holding: %1 · ends in %2").arg(hold_->reason, countdown_text(left));
}

QString HealthBar::hold_text() const { return hold_chip_->isHidden() ? QString() : hold_chip_->text(); }

void HealthBar::render_hold() {
  if (!hold_) {
    hold_chip_->clear();
    hold_chip_->hide();
    return;
  }
  hold_chip_->setText(hold_text_at(now_()));
  style::set_chip(hold_chip_, style::Level::Warning);
  hold_chip_->show();
}

HealthBar::Status HealthBar::status_of(const TransportHealth& health) {
  if (!health.connected) {
    return Status::Down;
  }
  return health.error_count == 0 ? Status::Ok : Status::Degraded;
}

style::Level HealthBar::level_of(Status status) {
  switch (status) {
    case Status::Ok:
      return style::Level::Ok;
    case Status::Degraded:
      return style::Level::Warning;
    case Status::Down:
      return style::Level::Error;
    case Status::Unknown:
      break;
  }
  return style::Level::Unknown;
}

std::optional<HealthBar::Status> HealthBar::status(const std::string& transport) const {
  auto it = chips_.find(transport);
  if (it == chips_.end()) {
    return std::nullopt;
  }
  return shown_status(it->second);
}

QLabel* HealthBar::chip(const std::string& transport) const {
  auto it = chips_.find(transport);
  return it == chips_.end() ? nullptr : it->second.label;
}

HealthBar::Chip& HealthBar::chip_for(const std::string& transport) {
  Chip& chip = chips_[transport];
  if (!chip.label) {
    chip.label = new QLabel(this);
    chip.label->setObjectName(QString::fromStdString(transport));
    chip.label->setMargin(2);
    layout_->addWidget(chip.label);
  }
  return chip;
}

void HealthBar::render(const std::string& name, const Chip& chip) {
  QString text = QString::fromStdString(name);
  if (chip.last_ok) {
    const auto age = std::chrono::duration_cast<std::chrono::seconds>(now_() - *chip.last_ok);
    text += QStringLiteral(" · ") + age_text(std::max(age, std::chrono::seconds{0}));
  }
  chip.label->setText(text);
  // Explicit rich text, every interpolated field escaped: an error text with
  // "<" must neither swallow the rest nor render as markup.
  const auto html = [](const QString& plain) { return plain.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")); };
  QString tip = html(chip.transport_tip.isEmpty() ? QString::fromStdString(name) : chip.transport_tip);
  for (const auto& [device, d] : chip.devices) {
    const auto state = to_string(d.state);
    QString line = QStringLiteral("%1: %2").arg(QString::fromStdString(device),
                                                QString::fromUtf8(state.data(), static_cast<qsizetype>(state.size())));
    if (d.state != DeviceState::Ok) {
      line += QStringLiteral(", %1 failure(s) in a row").arg(d.failures);
      if (!d.error.empty()) {
        line += QStringLiteral(", last error: ") + QString::fromStdString(d.error);
      }
      if (d.last_ok) {
        const auto age = std::chrono::duration_cast<std::chrono::seconds>(now_() - *d.last_ok);
        line += QStringLiteral(", last ok ") + age_text(std::max(age, std::chrono::seconds{0})) + QStringLiteral(" ago");
      } else {
        line += QStringLiteral(", never ok");
      }
    }
    tip += QStringLiteral("<br>") + html(line);
  }
  tip = QStringLiteral("<qt>") + tip + QStringLiteral("</qt>");
  // Only when it changed: render runs every second, and setting a tooltip
  // that is showing resets it.
  if (chip.label->toolTip() != tip) {
    chip.label->setToolTip(tip);
  }
  style::set_chip(chip.label, level_of(shown_status(chip)));
}

}  // namespace nobion::ui
