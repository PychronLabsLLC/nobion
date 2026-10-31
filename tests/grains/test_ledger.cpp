#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <span>
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
  // The same script at +v and -v in two ledgers gives the same tallies.
  PassLedger other{GrainOptions{}};
  int f = 0;
  const auto run = [&](PassLedger& l, double sgn, int n, double ref_v, double grain_v) {
    for (int i = 0; i < n; ++i, ++f) {
      const PassLedger::Sample sample{1, sgn * grain_v, 0.1};
      l.update(pychron::TimePoint{} + std::chrono::milliseconds(50 * f), sgn * ref_v, 0.1,
               std::span<const PassLedger::Sample>(&sample, 1));
    }
  };
  for (int pass = 0; pass < 2; ++pass) {
    run(ledger, 1.0, 10, 5.0, pass == 0 ? 3.0 : -3.0);
    run(ledger, 1.0, 1, 0.0, 0.0);
    run(other, -1.0, 10, 5.0, pass == 0 ? 3.0 : -3.0);
    run(other, -1.0, 1, 0.0, 0.0);
  }
  const Tally a = ledger.tally(1);
  const Tally b = other.tally(1);
  EXPECT_EQ(a.same_passes, 1);
  EXPECT_EQ(a.opposite_passes, 1);
  EXPECT_EQ(a.same_passes, b.same_passes);
  EXPECT_EQ(a.opposite_passes, b.opposite_passes);
  EXPECT_EQ(a.none_passes, b.none_passes);
  EXPECT_DOUBLE_EQ(a.same_weight, b.same_weight);
  EXPECT_DOUBLE_EQ(a.opposite_weight, b.opposite_weight);
}

TEST_F(Ledger, GrainInOneFrameOfAPassCastsNoVote) {
  for (int i = 0; i < 10; ++i) {
    PassLedger::Sample sample{1, -50.0, 0.1};
    const std::span<const PassLedger::Sample> in =
        i == 4 ? std::span<const PassLedger::Sample>(&sample, 1)
               : std::span<const PassLedger::Sample>();
    ledger.update(now(), 5.0, 0.1, in);
    ++frame;
  }
  stop();
  const Tally t = ledger.tally(1);
  EXPECT_EQ(t.same_passes + t.opposite_passes + t.none_passes, 0);
}

TEST_F(Ledger, GrainAppearingMidPassStillVotes) {
  for (int i = 0; i < 12; ++i) {
    const PassLedger::Sample sample{1, 3.0, 0.1};
    const std::span<const PassLedger::Sample> in =
        i >= 5 ? std::span<const PassLedger::Sample>(&sample, 1)
               : std::span<const PassLedger::Sample>();
    ledger.update(now(), 5.0, 0.1, in);
    ++frame;
  }
  stop();
  EXPECT_EQ(ledger.tally(1).same_passes, 1);
}

TEST_F(Ledger, LongGapBeforeAPassDoesNotDominate) {
  // An idle spell, then a pass whose grain moves opposite to the reference.
  stop();
  frame += 100000;
  feed(10, 5.0, -3.0);
  stop();
  EXPECT_EQ(ledger.tally(1).opposite_passes, 1);
  EXPECT_EQ(ledger.tally(1).same_passes, 0);
}

TEST_F(Ledger, BackwardTimestampDoesNotFlipTheVote) {
  feed(5, 5.0, 3.0);
  frame -= 40;  // clock steps back
  feed(5, 5.0, 3.0);
  frame += 40;
  stop();
  EXPECT_EQ(ledger.tally(1).same_passes, 1);
  EXPECT_EQ(ledger.tally(1).opposite_passes, 0);
}

TEST_F(Ledger, NonFiniteSamplesCastNoWrongVote) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  feed(10, 5.0, 3.0);
  stop();
  ASSERT_EQ(ledger.tally(1).same_passes, 1);
  // A grain whose every sample is NaN votes nothing; a NaN reference ends the pass.
  feed(10, 5.0, nan);
  stop();
  EXPECT_EQ(ledger.tally(1).opposite_passes, 0);
  EXPECT_EQ(ledger.tally(1).same_passes, 1);
  feed(10, 5.0, 3.0, 0.1, nan);
  stop();
  EXPECT_EQ(ledger.tally(1).opposite_passes, 0);
  feed(5, 5.0, 3.0);
  feed(1, nan, 3.0);
  EXPECT_FALSE(ledger.pass_running());
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
