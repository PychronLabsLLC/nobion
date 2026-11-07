// The flight recorder's byte ring (spec 2026-11-06-field-diagnostics, 5.2).

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

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

// TSan cannot know that a lock-free read of the ring is meant to race with
// its writers (the crash handler accepts a torn line): the reader below tells
// it so, for its copy only.
#if defined(__SANITIZE_THREAD__)
#define BYTE_RING_TSAN 1
#elif defined(__has_feature) && !defined(__CPPCHECK__)
#if __has_feature(thread_sanitizer)
#define BYTE_RING_TSAN 1
#endif
#endif
#ifdef BYTE_RING_TSAN
extern "C" void AnnotateIgnoreReadsBegin(const char* file, int line);
extern "C" void AnnotateIgnoreReadsEnd(const char* file, int line);
#endif

namespace {

// A copy of the ring's spans made without its lock, as the crash handler
// reads them, cut to the part no writer can have touched meanwhile: bytes
// from the tail as it stands after the copy (a writer moves the tail past
// what it is about to overwrite before writing). Whole lines, possibly none.
std::string lock_free_copy(const ByteRing& ring) {
  const auto before = ring.spans();
  std::string copy;
  copy.reserve(before.first.size() + before.second.size());
#ifdef BYTE_RING_TSAN
  AnnotateIgnoreReadsBegin(__FILE__, __LINE__);
#endif
  copy.append(before.first.data(), before.first.size());
  copy.append(before.second.data(), before.second.size());
#ifdef BYTE_RING_TSAN
  AnnotateIgnoreReadsEnd(__FILE__, __LINE__);
#endif
  std::atomic_thread_fence(std::memory_order_acquire);  // the copy is read before the tail is
  const auto after = ring.spans();
  if (after.begin >= before.end) return {};
  const auto torn = static_cast<std::size_t>(after.begin > before.begin ? after.begin - before.begin : 0);
  return copy.substr(torn);
}

// `w<t> <i> <x...> end`
bool written_line(std::string_view line) {
  if (line.size() < 8 || line[0] != 'w' || line[1] < '0' || line[1] > '9' || line[2] != ' ') return false;
  if (!line.ends_with(" end")) return false;
  std::size_t i = 3;
  const std::size_t digits = i;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
  if (i == digits || i >= line.size() || line[i] != ' ') return false;
  ++i;
  const std::size_t pad_end = line.size() - 4;
  for (; i < pad_end; ++i) {
    if (line[i] != 'x') return false;
  }
  return true;
}

}  // namespace

// Several writers wrap a small ring thousands of times while a reader copies
// its spans with no lock: every line in the untouched part of each copy is
// whole and one a writer wrote; a snapshot (under the lock) never has a
// torn line at all.
TEST(ByteRing, ConcurrentWritersAndALockFreeReader) {
  ByteRing ring(4096);
  constexpr int kWriters = 4;
  constexpr int kLines = 20000;
  std::atomic<int> done{0};
  std::vector<std::thread> writers;
  writers.reserve(kWriters);
  for (int t = 0; t < kWriters; ++t) {
    writers.emplace_back([&ring, &done, t] {
      std::string line;
      for (int i = 0; i < kLines; ++i) {
        line = "w" + std::to_string(t) + " " + std::to_string(i) + " ";
        line.append(static_cast<std::size_t>(i % 41), 'x');
        line.append(" end\n");
        ring.append(line);
      }
      done.fetch_add(1);
    });
  }
  std::size_t checked = 0;
  std::size_t copies = 0;
  do {  // at least once, however soon the writers finish
    const std::string copy = lock_free_copy(ring);
    ++copies;
    std::string_view rest(copy);
    while (!rest.empty()) {
      const std::size_t end = rest.find('\n');
      ASSERT_NE(end, std::string_view::npos) << "a copy that does not end with a line break";
      const std::string_view line = rest.substr(0, end);
      ASSERT_TRUE(written_line(line)) << "torn or foreign line: " << line;
      ++checked;
      rest.remove_prefix(end + 1);
    }
    const std::string snap = ring.snapshot();
    std::string_view lines(snap);
    while (!lines.empty()) {
      const std::size_t end = lines.find('\n');
      ASSERT_NE(end, std::string_view::npos);
      ASSERT_TRUE(written_line(lines.substr(0, end))) << lines.substr(0, end);
      lines.remove_prefix(end + 1);
    }
  } while (done.load() < kWriters);
  for (auto& w : writers) w.join();
  EXPECT_GT(copies, 0u);
  EXPECT_GT(checked, 0u);
}
