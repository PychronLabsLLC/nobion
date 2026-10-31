#pragma once

#include <QObject>

namespace pychron::grains::test {

class ReplayCommandTest : public QObject {
  Q_OBJECT
 private slots:
  void aRecordingReplaysClean();
  void aReportThatDiffersExitsNonZero();
  void aMissingFolderExitsNonZero();
};

}  // namespace pychron::grains::test
