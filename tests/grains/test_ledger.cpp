#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "pychron/grains/ledger.hpp"

using namespace pychron::grains;

namespace {

constexpr double kDt = 0.05;  // 20 fps

class Ledger : public ::testing::Test {
 protected:
  // Feeds n frames of constant velocities for grain 1.
  void feed(int n, double ref_v, double grain_v, double ref_c = 0.1, double grain_c = 0.1) {
    for (int i = 0; i < n; ++i) {
      const PassLedger::Sample sample{1, grain_v, grain_c};
      ledger.update(now(), ref_v, ref_c, std::span<const PassLedger::Sample>(&sample, 1));
      ++frame;
    }
  }
  pychron::TimePoint now() const {
    return pychron::TimePoint{} +
           std::chrono::duration_cast<pychron::TimePoint::duration>(
               std::chrono::duration<double>(frame * kDt));
  }
  // A stopped reference ends the pass in progress.
  void stop() {
    ledger.update(now(), 0.0, 0.1, {});
    ++frame;
  }
  PassLedger ledger{GrainOptions{}};
  int frame = 0;
};

}  // namespace

TEST_F(Ledger, SameDirectionVotesSame) {
  feed(10, 5.0, 3.0);
  EXPECT_TRUE(ledger.pass_running());
  stop();
  EXPECT_FALSE(ledger.pass_running());
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.same_passes, 1);
  EXPECT_EQ(t.opposite_passes, 0);
  EXPECT_EQ(t.none_passes, 0);
  EXPECT_GT(t.same_weight, 0.0);
  EXPECT_EQ(ledger.last_vote(1), Vote::Same);
}

TEST_F(Ledger, OppositeDirectionVotesOpposite) {
  feed(10, 5.0, -3.0);
  stop();
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.opposite_passes, 1);
  EXPECT_EQ(t.same_passes, 0);
  EXPECT_GT(t.opposite_weight, 0.0);
  EXPECT_EQ(ledger.last_vote(1), Vote::Opposite);
}

TEST_F(Ledger, FlippingBothSignsLeavesTheVote) {
  feed(10, -5.0, -3.0);
  stop();
  EXPECT_EQ(ledger.last_vote(1), Vote::Same);
  feed(10, -5.0, 3.0);
  stop();
  EXPECT_EQ(ledger.last_vote(1), Vote::Opposite);
  EXPECT_EQ(ledger.tally(1).same_passes, 1);
  EXPECT_EQ(ledger.tally(1).opposite_passes, 1);
}

TEST_F(Ledger, LowContrastVotesNone) {
  feed(10, 5.0, 3.0, 0.1, 0.005);
  stop();
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.none_passes, 1);
  EXPECT_EQ(t.same_passes, 0);
  EXPECT_DOUBLE_EQ(t.same_weight, 0.0);
  EXPECT_EQ(ledger.last_vote(1), Vote::None);
}

TEST_F(Ledger, WeightClampedToOne) {
  feed(10, 5.0, 3.0, 0.05, 0.2);
  stop();
  EXPECT_DOUBLE_EQ(ledger.tally(1).same_weight, 1.0);
}

TEST_F(Ledger, WeightIsContrastRatio) {
  feed(10, 5.0, 3.0, 0.2, 0.1);
  stop();
  EXPECT_NEAR(ledger.tally(1).same_weight, 0.5, 1e-12);
}

TEST_F(Ledger, ShortPassDiscarded) {
  feed(4, 5.0, 3.0);
  stop();
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.same_passes + t.opposite_passes + t.none_passes, 0);
  EXPECT_EQ(ledger.last_vote(1), Vote::None);
}

TEST_F(Ledger, StoppedReferenceClosesPass) {
  feed(10, 5.0, 3.0);
  EXPECT_TRUE(ledger.pass_running());
  feed(1, 0.5, 3.0);  // below min_ref_speed_px_s
  EXPECT_FALSE(ledger.pass_running());
  EXPECT_EQ(ledger.tally(1).same_passes, 1);
}

TEST_F(Ledger, SignChangeClosesPassAndStartsNext) {
  feed(10, 5.0, 3.0);
  feed(10, -5.0, 3.0);
  EXPECT_TRUE(ledger.pass_running());
  EXPECT_EQ(ledger.tally(1).same_passes, 1);
  stop();
  EXPECT_EQ(ledger.tally(1).opposite_passes, 1);
}

TEST_F(Ledger, DropPassCastsNoVote) {
  feed(10, 5.0, 3.0);
  ledger.drop_pass();
  EXPECT_FALSE(ledger.pass_running());
  stop();
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.same_passes + t.opposite_passes + t.none_passes, 0);
}

TEST_F(Ledger, IrregularSpeedOneVote) {
  const std::vector<double> speeds = {2.0, 6.0, 3.0, 9.0, 1.5, 4.0, 7.0, 2.5};
  for (const double v : speeds) {
    feed(1, v, v * 0.5);
  }
  stop();
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.same_passes, 1);
  EXPECT_EQ(t.opposite_passes + t.none_passes, 0);
}

TEST_F(Ledger, ForgetClearsTally) {
  feed(10, 5.0, 3.0);
  stop();
  ASSERT_EQ(ledger.tally(1).same_passes, 1);
  ledger.forget(1);
  EXPECT_EQ(ledger.tally(1).same_passes, 0);
  EXPECT_EQ(ledger.last_vote(1), Vote::None);
}

TEST_F(Ledger, UnknownIdIsZeros) {
  const Tally t = ledger.tally(99);
  EXPECT_EQ(t.same_passes + t.opposite_passes + t.none_passes, 0);
  EXPECT_DOUBLE_EQ(t.same_weight + t.opposite_weight, 0.0);
}

TEST_F(Ledger, ResetClearsEverything) {
  feed(10, 5.0, 3.0);
  stop();
  feed(3, 5.0, 3.0);
  ledger.reset();
  EXPECT_FALSE(ledger.pass_running());
  EXPECT_EQ(ledger.tally(1).same_passes, 0);
}
