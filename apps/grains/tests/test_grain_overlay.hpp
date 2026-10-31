#pragma once

#include <QObject>

namespace pychron::grains::test {

class GrainOverlayTest : public QObject {
  Q_OBJECT
 private slots:
  void candidateDrawsSanidineQuestion();
  void noReferenceHasNoLabels();
  void textPresentForEveryLabel();
  void bannerShowsReferenceAndSide();
  void failedKeepsPictureGreyed();
  void toFrameInvertsToWidget();
  void pinnedGrainGreyed();
};

}  // namespace pychron::grains::test
