// HealthBar and device health: a transport's chip turns amber when one of its
// devices is Faulted, and its tooltip lists each device (spec field
// diagnostics section 7). CoreBridge relays DeviceHealthChanged to the UI
// thread.

#include <chrono>
#include <string>
#include <thread>

#include <QtTest/QtTest>

#include "core_bridge.hpp"
#include "health_bar.hpp"
#include "ui_fixture.hpp"

using namespace nobion;
using namespace std::chrono_literals;
using nobion::ui::HealthBar;

namespace {

DeviceHealthChanged change(const std::string& device, const std::string& transport, DeviceState from, DeviceState to,
                           std::uint64_t failures, const std::string& text, TimePoint ts) {
  DeviceHealthChanged e;
  e.device = device;
  e.transport = transport;
  e.from = from;
  e.to = to;
  e.consecutive_failures = failures;
  e.last_error_text = text;
  e.ts = ts;
  return e;
}

}  // namespace

class TestHealthBarDevices : public QObject {
  Q_OBJECT

 private slots:
  void transportUpWithAFaultedDeviceIsAmber() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 0, "", now});
    QCOMPARE(bar.chip_status(QStringLiteral("serial0")), HealthBar::Status::Ok);
    bar.update_device(change("pump", "serial0", DeviceState::Degraded, DeviceState::Faulted, 5, "timeout", now));
    QCOMPARE(bar.chip_status(QStringLiteral("serial0")), HealthBar::Status::Degraded);
    QCOMPARE(HealthBar::level_of(bar.chip_status(QStringLiteral("serial0"))), ui::style::Level::Warning);
    // A device never makes a down transport look better.
    bar.update_health(TransportHealth{"serial0", false, 1, "gone", now});
    QCOMPARE(bar.chip_status(QStringLiteral("serial0")), HealthBar::Status::Down);
    QCOMPARE(bar.chip_status(QStringLiteral("nowhere")), HealthBar::Status::Unknown);
  }

  void tooltipListsEachDevice() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 0, "", now});
    bar.update_device(change("gauge", "serial0", DeviceState::Unknown, DeviceState::Ok, 0, "", now));
    now += 90s;
    bar.update_device(change("pump", "serial0", DeviceState::Degraded, DeviceState::Faulted, 7, "read timed out", now));
    now += 30s;
    bar.refresh();
    const QString tip = bar.chip("serial0")->toolTip();
    QVERIFY2(tip.contains(QStringLiteral("gauge")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("pump")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("faulted")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("7")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("read timed out")), qPrintable(tip));
    // gauge was last ok 2 minutes ago on the injected clock.
    QVERIFY2(tip.contains(QStringLiteral("2m ago")), qPrintable(tip));
  }

  void recoveredDeviceClearsAmber() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 0, "", now});
    bar.update_device(change("pump", "serial0", DeviceState::Degraded, DeviceState::Faulted, 5, "x", now));
    QCOMPARE(bar.chip_status(QStringLiteral("serial0")), HealthBar::Status::Degraded);
    bar.update_device(change("pump", "serial0", DeviceState::Faulted, DeviceState::Ok, 0, "", now));
    QCOMPARE(bar.chip_status(QStringLiteral("serial0")), HealthBar::Status::Ok);
  }

  void bridgeRelaysDeviceHealthToTheUiThread() {
    auto line = ui::test::make_example_line();
    ui::CoreBridge bridge(*line);
    QThread* seen_on = nullptr;
    QString device;
    connect(&bridge, &ui::CoreBridge::deviceHealth, this, [&](const DeviceHealthChanged& e) {
      seen_on = QThread::currentThread();
      device = QString::fromStdString(e.device);
    });
    std::thread([&] {
      line->bus().publish(change("pump", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "e", TimePoint{}));
    }).join();
    QTRY_COMPARE(device, QStringLiteral("pump"));
    QCOMPARE(seen_on, QThread::currentThread());
    line->stop();
  }
};

QTEST_MAIN(TestHealthBarDevices)
#include "test_health_bar_devices.moc"
