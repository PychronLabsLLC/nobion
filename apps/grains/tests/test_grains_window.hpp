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
};

}  // namespace pychron::grains::test
