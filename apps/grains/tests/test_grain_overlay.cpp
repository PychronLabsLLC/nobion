#include "test_grain_overlay.hpp"

#include <algorithm>
#include <cmath>

#include <QImage>
#include <QPixmap>
#include <QTest>

#include "grain_overlay.hpp"
#include "overlay_tokens.hpp"
#include "pychron/grains/types.hpp"
#include "pychron/vision/frame.hpp"

namespace pychron::grains::test {

namespace {

GrainView square(std::uint64_t id, double cx, double cy, Label label) {
  GrainView g;
  g.id = id;
  g.label = label;
  g.score = 0.9;
  g.passes_agree = 3;
  g.outline = {{cx + 10, cy}, {cx, cy + 10}, {cx - 10, cy}, {cx, cy - 10}};
  return g;
}

GrainReport report(ReferenceState state, std::vector<GrainView> grains) {
  GrainReport r;
  r.reference = state;
  r.grains = std::move(grains);
  return r;
}

vision::Frame picture() { return vision::Frame::make(100, 80, 255, 200); }

}  // namespace

void GrainOverlayTest::candidateDrawsSanidineQuestion() {
  GrainOverlay overlay;
  overlay.resize(400, 320);
  GrainView reference = square(1, 20, 20, Label::Candidate);
  reference.reference = true;
  overlay.show_frame(picture(), report(ReferenceState::Reading, {reference, square(2, 60, 40, Label::Candidate)}));
  const QStringList labels = overlay.labels();
  QVERIFY(std::ranges::any_of(labels, [](const QString& s) { return s.contains(QStringLiteral("sanidine?")); }));
  QVERIFY(std::ranges::none_of(labels, [](const QString& s) {
    const qsizetype at = s.indexOf(QStringLiteral("sanidine"));
    return at >= 0 && !s.mid(at).startsWith(QStringLiteral("sanidine?"));
  }));
}

void GrainOverlayTest::noReferenceHasNoLabels() {
  GrainOverlay overlay;
  overlay.resize(400, 320);
  // labels the pipeline should never have given: none shown all the same
  overlay.show_frame(picture(), report(ReferenceState::None, {square(1, 20, 20, Label::Candidate)}));
  QVERIFY(overlay.labels().isEmpty());
  overlay.show_frame(picture(), report(ReferenceState::Disagree, {square(1, 20, 20, Label::Candidate)}));
  QVERIFY(overlay.labels().isEmpty());
  QVERIFY(overlay.banner_text().contains(QStringLiteral("disagree")));
}

void GrainOverlayTest::textPresentForEveryLabel() {
  GrainOverlay overlay;
  overlay.resize(400, 320);
  overlay.show_frame(picture(), report(ReferenceState::Reading,
                                       {square(1, 20, 20, Label::Candidate), square(2, 60, 20, Label::Not), square(3, 20, 60, Label::Unsure)}));
  const auto drawn = overlay.drawn_labels();
  QCOMPARE(drawn.size(), std::size_t{3});
  for (const DrawnLabel& label : drawn) {
    QVERIFY(!label.text.isEmpty());
  }
  QVERIFY(drawn[1].text.contains(QStringLiteral("not")));
  QVERIFY(drawn[2].text.contains(QStringLiteral("unsure")));
  // colour is never the only signal, and each label has its own
  QCOMPARE(drawn[0].colour, tokens::kCandidate);
  QCOMPARE(drawn[1].colour, tokens::kNot);
  QCOMPARE(drawn[2].colour, tokens::kUnsure);
}

void GrainOverlayTest::bannerShowsReferenceAndSide() {
  GrainOverlay overlay;
  overlay.resize(400, 320);
  GrainView reference = square(3, 20, 20, Label::Candidate);
  reference.reference = true;
  overlay.set_side(Side::Below);
  overlay.show_frame(picture(), report(ReferenceState::Reading, {reference}));
  QCOMPARE(overlay.banner_text(), QStringLiteral("reference 3, below oil: reading"));
  overlay.set_side(Side::Above);
  QVERIFY(overlay.banner_text().contains(QStringLiteral("reference 3, above oil")));
  // the id is kept while the reference is lost
  overlay.show_frame(picture(), report(ReferenceState::Lost, {}));
  QVERIFY(overlay.banner_text().contains(QStringLiteral("reference 3, above oil: lost")));
  GrainReport slow = report(ReferenceState::Reading, {reference});
  slow.warning = "camera too slow";
  overlay.show_frame(picture(), slow);
  QVERIFY(overlay.banner_text().contains(QStringLiteral("camera too slow")));
  overlay.set_notice(QStringLiteral("recording failed"));
  QVERIFY(overlay.banner_text().contains(QStringLiteral("recording failed")));
}

void GrainOverlayTest::failedKeepsPictureGreyed() {
  GrainOverlay overlay;
  overlay.resize(400, 320);
  GrainView reference = square(1, 20, 20, Label::Candidate);
  reference.reference = true;
  overlay.show_frame(picture(), report(ReferenceState::Reading, {reference, square(2, 60, 40, Label::Candidate)}));
  const QImage before = overlay.grab().toImage();
  overlay.set_failed(QStringLiteral("camera lost"));
  const QImage after = overlay.grab().toImage();
  QVERIFY(overlay.has_picture());
  QVERIFY(overlay.failed());
  QVERIFY(overlay.banner_text().contains(QStringLiteral("camera lost")));
  // a corner of the picture, clear of text: darker under the veil
  const QPoint corner(8, 330 - 20);
  QVERIFY(qGray(after.pixel(corner)) < qGray(before.pixel(corner)));
  // the labels are held, greyed
  for (const DrawnLabel& label : overlay.drawn_labels()) {
    QCOMPARE(label.colour, tokens::kGreyed);
  }
  QVERIFY(!overlay.drawn_labels().empty());
}

void GrainOverlayTest::toFrameInvertsToWidget() {
  GrainOverlay overlay;
  overlay.show_frame(picture(), report(ReferenceState::None, {}));
  for (const QSize size : {QSize(400, 320), QSize(500, 200), QSize(200, 500)}) {
    overlay.resize(size);
    for (const QPointF p : {QPointF(0, 0), QPointF(10.5, 20.25), QPointF(99, 79)}) {
      const QPointF back = overlay.to_frame(overlay.to_widget(p));
      QVERIFY(std::abs(back.x() - p.x()) < 1e-9);
      QVERIFY(std::abs(back.y() - p.y()) < 1e-9);
    }
  }
  // a click lands on the pixel under it
  overlay.resize(400, 320);
  const QPointF centre = overlay.to_frame(QPointF(200, 160));
  QVERIFY(std::abs(centre.x() - 49.5) < 1.0);
  QVERIFY(std::abs(centre.y() - 39.5) < 1.0);
}

void GrainOverlayTest::pinnedGrainGreyed() {
  GrainOverlay overlay;
  overlay.resize(400, 320);
  GrainView reference = square(1, 20, 20, Label::Candidate);
  reference.reference = true;
  overlay.show_frame(picture(), report(ReferenceState::Reading, {reference, square(2, 60, 40, Label::Candidate), square(3, 30, 60, Label::Not)}));
  overlay.set_picked(2, true);
  QVERIFY(overlay.is_picked(2));
  for (const DrawnLabel& label : overlay.drawn_labels()) {
    if (label.id == 2) {
      QCOMPARE(label.colour, tokens::kGreyed);
      QVERIFY(label.text.contains(QStringLiteral("picked")));
    } else if (label.id == 3) {
      QCOMPARE(label.colour, tokens::kNot);
    }
  }
  overlay.set_picked(2, false);
  const auto drawn = overlay.drawn_labels();
  const auto two = std::ranges::find_if(drawn, [](const DrawnLabel& l) { return l.id == 2; });
  QVERIFY(two != drawn.end());
  QCOMPARE(two->colour, tokens::kCandidate);
}

}  // namespace pychron::grains::test
