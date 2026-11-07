// The flight recorder's byte ring (spec 2026-11-06-field-diagnostics, 5.2).

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "byte_ring.hpp"

using nobion::detail::ByteRing;

namespace {

std::string line_of(int i) { return "line " + std::to_string(i) + "\n"; }

}  // namespace

TEST(ByteRing, KeepsWholeNewestLines) {
  ByteRing ring(64);
  for (int i = 0; i < 40; ++i) ring.append(line_of(i));
  const std::string snap = ring.snapshot();
  ASSERT_FALSE(snap.empty());
  // Whole lines only: it starts where a line starts and ends with a break.
  EXPECT_EQ(snap.rfind("line ", 0), 0u) << snap;
  EXPECT_EQ(snap.back(), '\n');
  // The newest line is last, and the ones before it are its predecessors.
  EXPECT_TRUE(snap.ends_with(line_of(39))) << snap;
  std::string expected;
  for (int i = 39; i >= 0 && expected.size() + line_of(i).size() <= 64; --i) expected.insert(0, line_of(i));
  EXPECT_EQ(snap, expected);
}

TEST(ByteRing, AddsAMissingLineBreak) {
  ByteRing ring(64);
  ring.append("one");
  ring.append("two\n");
  EXPECT_EQ(ring.snapshot(), "one\ntwo\n");
}

TEST(ByteRing, NeverExceedsItsSize) {
  ByteRing ring(100);
  for (int i = 0; i < 500; ++i) {
    ring.append(std::string(static_cast<std::size_t>(i % 37), 'x') + "\n");
    const std::string snap = ring.snapshot();
    EXPECT_LE(snap.size(), ring.capacity());
  }
  // A line longer than the ring keeps what fits, with its line break.
  ring.append(std::string(300, 'y') + "\n");
  const std::string snap = ring.snapshot();
  EXPECT_EQ(snap, std::string(99, 'y') + "\n");
}

TEST(ByteRing, SpansAreReadableWithoutTheLock) {
  ByteRing ring(50);
  for (int i = 0; i < 23; ++i) ring.append(line_of(i));  // wraps the buffer
  const auto spans = ring.spans();
  EXPECT_FALSE(spans.second.empty()) << "expected the content to wrap";
  EXPECT_EQ(std::string(spans.first) + std::string(spans.second), ring.snapshot());
  EXPECT_LE(spans.first.size() + spans.second.size(), ring.capacity());
}

TEST(ByteRing, AnEmptyRingHasNoSpans) {
  const ByteRing ring(16);
  const auto spans = ring.spans();
  EXPECT_TRUE(spans.first.empty());
  EXPECT_TRUE(spans.second.empty());
  EXPECT_EQ(ring.snapshot(), "");
}
