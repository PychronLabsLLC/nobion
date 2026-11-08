// Help > Collect Diagnostics… (field diagnostics spec section 8): the live
// state is taken on the main thread, the bundle written on a worker, and the
// result said with the zip's path.

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <QAbstractButton>
#include <QAction>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMainWindow>
#include <QMessageBox>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include "brand.hpp"
#include "collect_diagnostics.hpp"
#include "core_bridge.hpp"
#include "ui_fixture.hpp"
#include "zip_entries.hpp"

namespace fs = std::filesystem;
using nobion::ui::DiagnosticsCollector;

class TestCollectDiagnostics : public QObject {
  Q_OBJECT

 private slots:
  void collectDiagnosticsWritesAZipWithLiveState() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const fs::path dir = tmp.path().toStdString();

    auto line = nobion::ui::test::make_example_line();
    nobion::ui::CoreBridge bridge(*line);
    std::thread publisher(
        [&line] { line->bus().publish(nobion::TransportHealth{"valve_bus", false, 3, "timed out", {}}); });
    publisher.join();
    QTRY_VERIFY_WITH_TIMEOUT(bridge.state().health.count("valve_bus") == 1, 5000);

    QMainWindow window;
    nobion::ui::brand::add_help_menu(&window);
    nobion::ui::DiagnosticsDeps deps;
    deps.out = dir / "out" / "bundle.zip";
    deps.log_dir = dir / "logs";
    deps.trace_dir = dir / "traces";
    deps.site_path = dir / "site.toml";
    deps.clock = &line->clock();
    deps.snapshot = [&bridge] {
      nobion::ui::LiveSources sources;
      sources.core = &bridge;
      return nobion::ui::live_state_json(sources);
    };
    auto* collector = new DiagnosticsCollector(&window, std::move(deps));
    QSignalSpy finished(collector, &DiagnosticsCollector::finished);

    auto* action = window.findChild<QAction*>(QStringLiteral("collectDiagnostics"));
    QVERIFY(action != nullptr);
    QCOMPARE(action->text(), QStringLiteral("Collect Diagnostics…"));
    action->trigger();
    QVERIFY(finished.wait(10000) || finished.count() == 1);
    QCOMPARE(finished.count(), 1);
    const QString path = finished.at(0).at(0).toString();
    QVERIFY2(finished.at(0).at(1).toString().isEmpty(), qPrintable(finished.at(0).at(1).toString()));
    QCOMPARE(fs::path(path.toStdString()), dir / "out" / "bundle.zip");
    QVERIFY(!collector->busy());

    const auto entries = nobion::testing::zip_entries(path.toStdString());
    QVERIFY(entries.has_value());
    QVERIFY(entries->contains("README.txt"));
    QVERIFY(entries->contains("live.json"));
    const auto live = QJsonDocument::fromJson(QByteArray::fromStdString(entries->at("live.json")));
    QVERIFY2(live.isObject(), entries->at("live.json").c_str());
    QVERIFY(live.object().contains(QStringLiteral("transports")));
    const auto bus = live.object().value(QStringLiteral("transports")).toObject().value(QStringLiteral("valve_bus"));
    QVERIFY2(bus.isObject(), entries->at("live.json").c_str());
    QCOMPARE(bus.toObject().value(QStringLiteral("connected")).toBool(true), false);
    QCOMPARE(bus.toObject().value(QStringLiteral("last_error")).toString(), QStringLiteral("timed out"));

    // Where the zip is, with the offer to show it.
    auto* box = window.findChild<QMessageBox*>(QStringLiteral("collectDiagnosticsResult"));
    QVERIFY(box != nullptr);
    QVERIFY2(box->text().contains(path) || box->informativeText().contains(path), qPrintable(box->text()));
    QVERIFY(box->findChild<QAbstractButton*>(QStringLiteral("revealBundleButton")) != nullptr);
    box->close();
    line->stop();
  }

  // A zip that cannot be written is said, not thrown.
  void aFailureIsShownAsAMessage() {
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const fs::path dir = tmp.path().toStdString();
    // A file where the zip's folder would be.
    {
      std::ofstream(dir / "blocked") << "x";
    }
    QMainWindow window;
    nobion::ui::brand::add_help_menu(&window);
    nobion::ui::DiagnosticsDeps deps;
    deps.out = dir / "blocked" / "bundle.zip";
    deps.log_dir = dir / "logs";
    deps.trace_dir = dir / "traces";
    deps.site_path = dir / "site.toml";
    auto* collector = new DiagnosticsCollector(&window, std::move(deps));
    QSignalSpy finished(collector, &DiagnosticsCollector::finished);
    collector->collect();
    QVERIFY(finished.wait(10000) || finished.count() == 1);
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.at(0).at(0).toString().isEmpty());
    QVERIFY(!finished.at(0).at(1).toString().isEmpty());
    auto* box = window.findChild<QMessageBox*>(QStringLiteral("collectDiagnosticsResult"));
    QVERIFY(box != nullptr);
    QCOMPARE(box->icon(), QMessageBox::Warning);
    box->close();
  }
};

QTEST_MAIN(TestCollectDiagnostics)
#include "test_collect_diagnostics.moc"
