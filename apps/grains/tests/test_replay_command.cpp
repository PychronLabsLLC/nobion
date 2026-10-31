#include "test_replay_command.hpp"

#include <filesystem>
#include <sstream>

#include <QTemporaryDir>
#include <QTest>

#include "demo.hpp"
#include "pychron/grains/pipeline.hpp"
#include "pychron/grains/recording.hpp"
#include "pychron/grains/synth.hpp"
#include "replay_command.hpp"

namespace pychron::grains::test {

namespace {

// Recorded the way the runner does; `tamper` changes what is recorded.
std::filesystem::path record(const QTemporaryDir& root, bool tamper) {
  std::filesystem::path dir = std::filesystem::path(root.path().toStdString()) / (tamper ? "tampered" : "clean");
  const GrainOptions options;
  auto recording = GrainRecording::open(dir, options);
  if (!recording) {
    qWarning("open: %s", recording.error().what.c_str());
    return {};
  }
  GrainPipeline pipeline(options);
  GrainSynth synth(demo_scene(), triangle_z(2.0, 1.0), kDemoFps);
  for (int i = 0; i < 6; ++i) {
    auto frame = synth.grab();
    if (!frame) return {};
    GrainReport report = pipeline.process(*frame);
    if (tamper) {
      GrainView made_up;
      made_up.id = 99;
      report.grains.push_back(made_up);
    }
    if (!recording->add(*frame, report, {})) return {};
  }
  if (!recording->finish()) return {};
  return dir;
}

}  // namespace

void ReplayCommandTest::aRecordingReplaysClean() {
  const QTemporaryDir root;
  const auto dir = record(root, false);
  QVERIFY(!dir.empty());
  std::ostringstream out, err;
  QCOMPARE(run_replay(dir, out, err), 0);
  QVERIFY(out.str().find("mismatches 0") != std::string::npos);
  QVERIFY(err.str().empty());
}

void ReplayCommandTest::aReportThatDiffersExitsNonZero() {
  const QTemporaryDir root;
  const auto dir = record(root, true);
  QVERIFY(!dir.empty());
  std::ostringstream out, err;
  QCOMPARE(run_replay(dir, out, err), kReplayMismatched);
  QVERIFY(err.str().find("differ") != std::string::npos);
}

void ReplayCommandTest::aMissingFolderExitsNonZero() {
  const QTemporaryDir root;
  std::ostringstream out, err;
  QCOMPARE(run_replay(std::filesystem::path(root.path().toStdString()) / "none", out, err), kReplayFailed);
  QVERIFY(!err.str().empty());
}

}  // namespace pychron::grains::test
