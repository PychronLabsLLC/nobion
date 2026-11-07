#include "duration.hpp"

#include <chrono>

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace elctl {
namespace {

TEST(ParseDuration, AcceptsUnits) {
  EXPECT_EQ(*parse_duration("250ms"), nobion::Duration(250ms));
  EXPECT_EQ(*parse_duration("3s"), nobion::Duration(3s));
  EXPECT_EQ(*parse_duration("2m"), nobion::Duration(2min));
  EXPECT_EQ(*parse_duration("1h"), nobion::Duration(1h));
}

TEST(ParseDuration, BareNumberIsSeconds) {
  EXPECT_EQ(*parse_duration("5"), nobion::Duration(5s));
  EXPECT_EQ(*parse_duration("1.5"), nobion::Duration(1500ms));
}

TEST(ParseDuration, RejectsGarbage) {
  for (const char* bad : {"", "s", "abc", "5x", "-1s", "1.2.3s"}) {
    auto d = parse_duration(bad);
    ASSERT_FALSE(d) << bad;
    EXPECT_EQ(d.error().kind, nobion::ErrorKind::Config) << bad;
  }
}

}  // namespace
}  // namespace elctl
