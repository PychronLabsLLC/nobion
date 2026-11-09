// HealthBar and device health: a transport's chip turns amber when one of its
// devices is Faulted, and its tooltip lists each device (spec field
// diagnostics section 7). CoreBridge relays DeviceHealthChanged to the UI
// thread.

#include <chrono>
#include <string>
#include <thread>

#include <QSignalSpy>
#include <QTextDocument>
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
    bar.update_device(change("gauge", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "", now));
    now += 90s;
    bar.update_device(change("pump", "serial0", DeviceState::Degraded, DeviceState::Faulted, 7, "read timed out", now));
    now += 30s;
    bar.refresh();
    // The tooltip is rich text: compare what it shows, one line a device.
    QTextDocument doc;
    doc.setHtml(bar.chip("serial0")->toolTip());
    const QStringList lines = doc.toPlainText().split(QLatin1Char('\n'));
    const auto line_of = [&lines](const QString& device) {
      for (const QString& line : lines) {
        if (line.startsWith(device + QStringLiteral(": "))) return line;
      }
      return QString();
    };
    // gauge was last ok 2 minutes ago on the injected clock.
    QCOMPARE(line_of(QStringLiteral("gauge")),
             QStringLiteral("gauge: degraded, 1 failure(s) in a row, last ok 2m ago"));
    QCOMPARE(line_of(QStringLiteral("pump")),
             QStringLiteral("pump: faulted, 7 failure(s) in a row, last error: read timed out, never ok"));
  }

  void anEventWithNoTransportOrNoChipToTurnOkMakesNoChip() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_device(change("gauge", "", DeviceState::Unknown, DeviceState::Degraded, 1, "x", now));
    bar.update_device(change("pump", "spectrometer", DeviceState::Unknown, DeviceState::Ok, 0, "", now));
    QCOMPARE(bar.summary().unknown, 0);
    QCOMPARE(bar.summary().ok + bar.summary().degraded + bar.summary().down, 0);
    // A failure still makes one: the operator should see it.
    bar.update_device(change("pump", "spectrometer", DeviceState::Ok, DeviceState::Degraded, 1, "x", now));
    QCOMPARE(bar.chip_status(QStringLiteral("spectrometer")), HealthBar::Status::Degraded);
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

  void errorTextIsShownLiterallyInTheTooltip() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 1, "<i>t</i> & <", now});
    bar.update_device(change("pump", "serial0", DeviceState::Ok, DeviceState::Faulted, 3, "<b>x</b> & <", now));
    bar.update_device(change("zz", "serial0", DeviceState::Ok, DeviceState::Ok, 0, "", now));
    QTextDocument doc;
    doc.setHtml(bar.chip("serial0")->toolTip());
    const QString plain = doc.toPlainText();
    QVERIFY2(plain.contains(QStringLiteral("<b>x</b> & <")), qPrintable(plain));
    QVERIFY2(plain.contains(QStringLiteral("<i>t</i> & <")), qPrintable(plain));
    QVERIFY2(plain.contains(QStringLiteral("zz")), qPrintable(plain));
  }

  void aDeviceThatWasOkAndFailedShowsHowLongAgo() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_device(change("pump", "serial0", DeviceState::Ok, DeviceState::Faulted, 5, "boom", now));
    now += 45s;
    bar.refresh();
    const QString tip = bar.chip("serial0")->toolTip();
    QVERIFY2(tip.contains(QStringLiteral("45s ago")), qPrintable(tip));
    QVERIFY2(!tip.contains(QStringLiteral("never ok")), qPrintable(tip));
  }

  void aDeviceThatIsOkShowsNoAge() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 0, "", now});
    bar.update_device(change("pump", "serial0", DeviceState::Unknown, DeviceState::Ok, 0, "", now));
    now += 300s;
    bar.refresh();
    QTextDocument doc;
    doc.setHtml(bar.chip("serial0")->toolTip());
    const QStringList lines = doc.toPlainText().split(QLatin1Char('\n'));
    QVERIFY2(lines.contains(QStringLiteral("pump: ok")), qPrintable(doc.toPlainText()));
  }

  void degradedDeviceIsListedAndAmber() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 0, "", now});
    bar.update_device(change("pump", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "slow", now));
    QCOMPARE(bar.chip_status(QStringLiteral("serial0")), HealthBar::Status::Degraded);
    const QString tip = bar.chip("serial0")->toolTip();
    QVERIFY2(tip.contains(QStringLiteral("degraded")), qPrintable(tip));
    QVERIFY2(tip.contains(QStringLiteral("slow")), qPrintable(tip));
  }

  void devicesAreListedInNameOrder() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_device(change("pump", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "", now));
    bar.update_device(change("gauge", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "", now));
    bar.update_device(change("valve", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "", now));
    QTextDocument doc;
    doc.setHtml(bar.chip("serial0")->toolTip());
    const QString plain = doc.toPlainText();
    const auto g = plain.indexOf(QStringLiteral("gauge"));
    const auto p = plain.indexOf(QStringLiteral("pump"));
    const auto v = plain.indexOf(QStringLiteral("valve"));
    QVERIFY2(g >= 0 && g < p && p < v, qPrintable(plain));
  }

  void changedIsEmittedOnlyWhenTheSummaryChanges() {
    TimePoint now{};
    HealthBar bar(nullptr, [&now] { return now; });
    bar.update_health(TransportHealth{"serial0", true, 0, "", now});
    QSignalSpy spy(&bar, &HealthBar::changed);
    bar.update_device(change("pump", "serial0", DeviceState::Ok, DeviceState::Degraded, 1, "", now));
    QCOMPARE(spy.count(), 1);
    bar.update_device(change("pump", "serial0", DeviceState::Degraded, DeviceState::Faulted, 3, "", now));
    QCOMPARE(spy.count(), 1);
    bar.update_device(change("pump", "serial0", DeviceState::Faulted, DeviceState::Ok, 0, "", now));
    QCOMPARE(spy.count(), 2);
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
