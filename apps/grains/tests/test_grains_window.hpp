#pragma once

#include <QObject>

namespace pychron::grains::test {

class GrainsWindowTest : public QObject {
  Q_OBJECT
 private slots:
  void clickWithToolSetsReference();
  void refusedReferenceShowsReason();
  void recordingToggleWritesFolder();
  void noScoreTextSaysProbability();
  void secondReferenceButtonSetsOppositeReference();
  void pinButtonGreysTheSelectedGrain();
  void detailSaysWhyUnsure();
  void frameSizeChangeClearsPinsAndSelection();
  void pipelineResetClearsPinsAndSelection();
};

}  // namespace pychron::grains::test
