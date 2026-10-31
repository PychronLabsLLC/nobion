#include <gtest/gtest.h>

#include "pychron/grains/score.hpp"

using namespace pychron::grains;

TEST(Score, FormulaOfTheSpec) {
  EXPECT_DOUBLE_EQ(evidence_score(3, 0), 0.8);
  EXPECT_NEAR(evidence_score(3, 1), 4.0 / 6.0, 1e-12);
  EXPECT_NEAR(evidence_score(0, 0), 0.5, 1e-12);
}
TEST(Score, ThreeCleanPassesSameAsBelowReferenceIsCandidate) {
  EXPECT_EQ(decide({3, 0, 3, 0, 0}, Side::Below, {}), Label::Candidate);
}
TEST(Score, OppositeOfAboveReferenceIsCandidate) {
  EXPECT_EQ(decide({0, 3, 0, 3, 0}, Side::Above, {}), Label::Candidate);
}
TEST(Score, SameAsAboveReferenceIsNot) {
  EXPECT_EQ(decide({3, 0, 3, 0, 0}, Side::Above, {}), Label::Not);
}
TEST(Score, TwoPassesAreNotEnough) {
  EXPECT_EQ(decide({2, 0, 2, 0, 0}, Side::Below, {}), Label::Unsure);
}
TEST(Score, MixedVotesStayUnsure) {
  EXPECT_EQ(decide({3, 1, 3, 1, 0}, Side::Below, {}), Label::Unsure);
}
TEST(Score, TiedWeightsStayUnsure) {
  GrainOptions o;
  o.min_score = 0.1;
  EXPECT_EQ(decide({2, 2, 2, 2, 0}, Side::Below, o), Label::Unsure);
}
TEST(Score, NonePassesDoNotCount) {
  EXPECT_EQ(decide({0, 0, 0, 0, 9}, Side::Below, {}), Label::Unsure);
}
TEST(Score, NoBareSanidine) {
  EXPECT_EQ(label_text(Label::Candidate), "sanidine?");
}
