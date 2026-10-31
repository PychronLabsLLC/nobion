#include "test_grains_window.hpp"

#include <filesystem>

#include <QAbstractButton>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTest>
#include <QToolButton>

#include "demo.hpp"
#include "grain_overlay.hpp"
#include "grains_window.hpp"

namespace pychron::grains::test {

namespace {

// Frames of the demo scene at 8 times its speed: the worker reaches the
// click's frame within a fraction of a second.
constexpr double kSpeed = 8.0;
constexpr int kTimeoutMs = 30000;
constexpr QPointF kReferenceGrain{240, 175};
constexpr QPointF kEmptyPlace{160, 120};

void click_at(GrainsWindow& window, QPointF frame_px) {
  GrainOverlay* overlay = window.overlay();
  QTest::mouseClick(overlay, Qt::LeftButton, Qt::NoModifier, overlay->to_widget(frame_px).toPoint());
}

bool has_reference(const GrainOverlay& overlay) {
  const auto& grains = overlay.report().grains;
  return std::ranges::any_of(grains, [](const GrainView& g) { return g.reference; });
}

void show(GrainsWindow& window) {
  window.resize(1000, 640);
  window.show();
  QVERIFY2(QTest::qWaitForWindowExposed(&window), "window not exposed");
}

}  // namespace

void GrainsWindowTest::clickWithToolSetsReference() {
  GrainsWindow window(demo_poll(kSpeed), GrainOptions{});
  show(window);
  GrainOverlay* overlay = window.overlay();
  QTRY_VERIFY_WITH_TIMEOUT(overlay->report().seq >= 15, kTimeoutMs);
  auto* tool = window.findChild<QToolButton*>(QStringLiteral("reference_tool"));
  QVERIFY(tool);
  // A fringe takes a few frames to measure: a refusal is tried again.
  for (int attempt = 0; attempt < 40 && !has_reference(*overlay); ++attempt) {
    tool->setChecked(true);
    click_at(window, kReferenceGrain);
    QTest::qWait(150);
  }
  QVERIFY2(has_reference(*overlay), qPrintable(overlay->banner_text()));
  const auto& grains = overlay->report().grains;
  const auto reference = std::ranges::find_if(grains, [](const GrainView& g) { return g.reference; });
  QVERIFY(reference != grains.end());
  QCOMPARE(overlay->grain_at(kReferenceGrain).value_or(0), reference->id);
  QVERIFY(!tool->isChecked());
  QTRY_VERIFY_WITH_TIMEOUT(overlay->banner_text().contains(QStringLiteral("reference %1, below oil").arg(reference->id)), kTimeoutMs);
  // the grain of the same kind is a candidate, with its question mark
  QTRY_VERIFY_WITH_TIMEOUT(!overlay->labels().filter(QStringLiteral("sanidine?")).isEmpty(), kTimeoutMs);
}

void GrainsWindowTest::refusedReferenceShowsReason() {
  GrainsWindow window(demo_poll(kSpeed), GrainOptions{});
  show(window);
  GrainOverlay* overlay = window.overlay();
  QTRY_VERIFY_WITH_TIMEOUT(overlay->report().seq >= 3, kTimeoutMs);
  auto* tool = window.findChild<QToolButton*>(QStringLiteral("reference_tool"));
  QVERIFY(tool);
  tool->setChecked(true);
  click_at(window, kEmptyPlace);
  QTRY_VERIFY_WITH_TIMEOUT(overlay->banner_text().contains(QStringLiteral("no grain at that point")), kTimeoutMs);
  QVERIFY(!has_reference(*overlay));
  QVERIFY(overlay->banner_text().contains(QStringLiteral("no reference")));
}

void GrainsWindowTest::recordingToggleWritesFolder() {
  const QTemporaryDir folder;
  QVERIFY(folder.isValid());
  GrainsWindow window(demo_poll(kSpeed), GrainOptions{});
  show(window);
  GrainOverlay* overlay = window.overlay();
  window.set_record_folder(folder.path());
  QTRY_VERIFY_WITH_TIMEOUT(overlay->report().seq >= 3, kTimeoutMs);
  overlay->set_picked(1, true);
  auto* record = window.findChild<QAbstractButton*>(QStringLiteral("record"));
  QVERIFY(record);
  record->click();
  QVERIFY(record->isChecked());
  QVERIFY(!window.recording_path().isEmpty());
  // ids restart with the recording: pins are gone
  QVERIFY(!overlay->is_picked(1));
  const std::uint64_t start_seq = overlay->report().seq;
  QTRY_VERIFY_WITH_TIMEOUT(overlay->report().seq >= start_seq + 5, kTimeoutMs);
  record->click();
  QVERIFY(!record->isChecked());
  QVERIFY2(!overlay->banner_text().contains(QStringLiteral("fail")), qPrintable(overlay->banner_text()));
  const std::filesystem::path dir = window.recording_path().toStdString();
  QVERIFY(std::filesystem::is_directory(dir));
  QVERIFY(std::filesystem::exists(dir / "case.toml"));
  QVERIFY(std::filesystem::exists(dir / "tracks.jsonl"));
  QVERIFY(std::filesystem::path(dir).parent_path() == std::filesystem::path(folder.path().toStdString()));
}

void GrainsWindowTest::noScoreTextSaysProbability() {
  GrainsWindow window(demo_poll(kSpeed), GrainOptions{});
  show(window);
  GrainOverlay* overlay = window.overlay();
  QTRY_VERIFY_WITH_TIMEOUT(overlay->report().seq >= 15, kTimeoutMs);
  auto* tool = window.findChild<QToolButton*>(QStringLiteral("reference_tool"));
  QVERIFY(tool);
  for (int attempt = 0; attempt < 40 && !has_reference(*overlay); ++attempt) {
    tool->setChecked(true);
    click_at(window, kReferenceGrain);
    QTest::qWait(150);
  }
  QVERIFY(has_reference(*overlay));
  // select a candidate so the detail panel shows a score
  QTRY_VERIFY_WITH_TIMEOUT(!overlay->labels().isEmpty(), kTimeoutMs);
  click_at(window, QPointF(80, 65));
  QVERIFY(window.selected().has_value());
  QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QPlainTextEdit*>(QStringLiteral("detail"))->toPlainText().contains(QStringLiteral("score")), kTimeoutMs);

  QStringList texts;
  texts << window.windowTitle() << overlay->banner_text() << overlay->labels();
  for (const auto* w : window.findChildren<QWidget*>()) {
    texts << w->toolTip() << w->statusTip() << w->whatsThis();
    if (const auto* label = qobject_cast<const QLabel*>(w)) texts << label->text();
    if (const auto* button = qobject_cast<const QAbstractButton*>(w)) texts << button->text();
    if (const auto* edit = qobject_cast<const QPlainTextEdit*>(w)) texts << edit->toPlainText();
    if (const auto* combo = qobject_cast<const QComboBox*>(w)) {
      for (int i = 0; i < combo->count(); ++i) texts << combo->itemText(i);
    }
  }
  for (const QString& text : texts) {
    QVERIFY2(!text.contains(QStringLiteral("probab"), Qt::CaseInsensitive), qPrintable(text));
  }
  QVERIFY(std::ranges::any_of(texts, [](const QString& t) { return t.contains(QStringLiteral("score")); }));
}

}  // namespace pychron::grains::test
