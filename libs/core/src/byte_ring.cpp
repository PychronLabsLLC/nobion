#include "byte_ring.hpp"

#include <algorithm>
#include <cstring>

namespace nobion::detail {

ByteRing::ByteRing(std::size_t capacity)
    : capacity_(std::max<std::size_t>(capacity, 2)),
      // For overwrite: no byte is touched until written, so an untouched
      // ring costs address space, not memory.
      // NOLINTNEXTLINE(modernize-avoid-c-arrays,cppcoreguidelines-avoid-c-arrays): a raw byte buffer
      data_(std::make_unique_for_overwrite<char[]>(capacity_)) {}

void ByteRing::make_room(std::uint64_t incoming) noexcept {
  const std::uint64_t head = head_.load();
  std::uint64_t tail = tail_.load();
  while (head - tail + incoming > capacity_) {
    // The oldest line ends at the first '\n' at or after the tail: the head
    // always sits just past one, so the search never runs off the content.
    std::uint64_t at = tail;
    while (at < head) {
      const auto offset = static_cast<std::size_t>(at % capacity_);
      const auto run = static_cast<std::size_t>(std::min<std::uint64_t>(head - at, capacity_ - offset));
      const void* found = std::memchr(data_.get() + offset, '\n', run);
      if (found != nullptr) {
        at += static_cast<std::uint64_t>(static_cast<const char*>(found) - (data_.get() + offset)) + 1;
        break;
      }
      at += run;
    }
    tail = at;
  }
  tail_.store(tail);  // before the bytes it frees are overwritten
}

void ByteRing::copy_in(std::uint64_t at, const char* data, std::size_t size) noexcept {
  while (size > 0) {
    const auto offset = static_cast<std::size_t>(at % capacity_);
    const std::size_t run = std::min(size, capacity_ - offset);
    std::memcpy(data_.get() + offset, data, run);
    at += run;
    data += run;
    size -= run;
  }
}

void ByteRing::append(std::string_view line) noexcept {
  if (!line.empty() && line.back() == '\n') line.remove_suffix(1);
  line = line.substr(0, std::min(line.size(), capacity_ - 1));
  const std::uint64_t total = line.size() + 1;
  try {
    const std::lock_guard lock(mutex_);
    make_room(total);
    const std::uint64_t head = head_.load();
    copy_in(head, line.data(), line.size());
    copy_in(head + line.size(), "\n", 1);
    head_.store(head + total);  // after the bytes: a reader never sees unwritten ones
  } catch (...) {  // NOLINT(bugprone-empty-catch): a mutex that cannot be locked; the line is lost, not the record
  }
}

ByteRing::Spans ByteRing::spans() const noexcept {
  const std::uint64_t head = head_.load();
  std::uint64_t tail = tail_.load();
  // Read while an append runs, the two may be from different appends.
  tail = std::min(tail, head);
  if (head - tail > capacity_) tail = head - capacity_;
  const auto size = static_cast<std::size_t>(head - tail);
  const auto start = static_cast<std::size_t>(tail % capacity_);
  const std::size_t first = std::min(size, capacity_ - start);
  return {std::string_view(data_.get() + start, first), std::string_view(data_.get(), size - first)};
}

std::string ByteRing::snapshot() const {
  const std::lock_guard lock(mutex_);
  const Spans s = spans();
  std::string out;
  out.reserve(s.first.size() + s.second.size());
  out.append(s.first).append(s.second);
  return out;
}

}  // namespace nobion::detail
