#pragma once

// The flight recorder's memory (spec 2026-11-06-field-diagnostics, 5.2): one
// byte ring of whole lines, allocated once. Internal to libs/core.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace nobion::detail {

class ByteRing {
 public:
  // `capacity` bytes, allocated here and never again (at least 2).
  explicit ByteRing(std::size_t capacity);

  // Appends one line (`line` ends with '\n'; one is added if it does not).
  // The oldest whole lines are dropped to make room. A line longer than the
  // ring keeps its first capacity() - 1 bytes and its line break. The mutex is
  // held for the copy only. Never throws.
  void append(std::string_view line) noexcept;

  // Every line held, oldest first; whole lines only.
  [[nodiscard]] std::string snapshot() const;
  // The same lines appended to `out`, with no copy in between. `out` is
  // given room for capacity() more bytes before the lock is taken; under it
  // the lines are only copied.
  void append_snapshot_to(std::string& out) const;

  // The bytes held, as the two contiguous runs of the buffer they occupy
  // (the second empty unless the content wraps). Reads the two atomic indices
  // and takes no lock: for the crash handler, which may interrupt an append
  // (a line torn by a concurrent writer is accepted). No allocation.
  // `begin` and `end` are the stream positions of the bytes (every byte
  // ever appended counts): `begin` a line's start, `end` just past a line
  // break. A reader that copies the spans and then finds the tail (begin of
  // a later spans()) at or below `begin` knows no writer touched its copy;
  // bytes below a later tail may have been overwritten meanwhile.
  struct Spans {
    std::string_view first;
    std::string_view second;
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
  };
  [[nodiscard]] Spans spans() const noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  // Drops whole lines from the tail until `incoming` more bytes fit.
  // Caller holds mutex_.
  void make_room(std::uint64_t incoming) noexcept;
  void copy_in(std::uint64_t at, const char* data, std::size_t size) noexcept;

  const std::size_t capacity_;
  const std::unique_ptr<char[]> data_;  // NOLINT(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays): a raw byte buffer
  mutable std::mutex mutex_;            // writers, and snapshot()'s copy
  // Positions in the stream of every byte ever written; byte p is at
  // data_[p % capacity_]. tail_ is the start of the oldest line held, head_
  // one past the end of the newest. head_ - tail_ <= capacity_.
  std::atomic<std::uint64_t> head_{0};
  std::atomic<std::uint64_t> tail_{0};
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "the crash handler reads the indices");
};

}  // namespace nobion::detail
