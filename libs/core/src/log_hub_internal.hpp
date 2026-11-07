#pragma once

// Private bridge between Logger and LogHub::Impl (which is defined, with its
// spdlog members, in log_hub.cpp). Not installed; not part of the public API.

#include <cstdint>
#include <string>
#include <string_view>

#include "nobion/core/log_hub.hpp"

namespace nobion::detail {

// Current rule epoch (>= 1); changes on every LogHub::set_level.
std::uint64_t hub_rule_epoch(const LogHub::Impl& hub) noexcept;

// Level for `name` under the current rules, together with the epoch those
// rules belong to (read under the same lock, so the pair is consistent).
struct ResolvedLevel {
  LogLevel level;     // the file level
  LogLevel recorder;  // the recorder level
  std::uint64_t epoch;
};
ResolvedLevel hub_resolve_level(const LogHub::Impl& hub, std::string_view name) noexcept;

// The calling thread's label for a record: its LogContext tag, or `t<n>`
// (LogContext::thread_number()) when it has none. Read on the logging thread.
std::string thread_label();

// The string LogContext::Thread points the calling thread at, or null. For
// the crash handler, which reads data() and size() and nothing else. POSIX:
// read from a pthread key (pthread_getspecific; no allocation, unlike a first
// thread_local read on macOS); null until ensure_thread_tag_key() has run.
// LogContext::thread_tag() keeps reading the thread_local, which holds the
// same pointer.
const std::string* thread_tag_pointer() noexcept;

// Creates that key if it does not exist yet. Not from a signal handler.
void ensure_thread_tag_key() noexcept;

// LogHub::write on the Impl; a no-op once the hub has been destroyed.
// `to_ring` false: file, stderr and bus only (the record is in the ring already).
void hub_write(LogHub::Impl& hub, LogLevel level, std::string_view logger, std::string_view message,
               bool to_ring = true) noexcept;

// A record below the file level and at or above the recorder level. Neither
// file, stderr nor bus see it. Never throws.
void hub_record_only(LogHub::Impl& hub, LogLevel level, std::string_view logger,
                     std::string_view message) noexcept;

}  // namespace nobion::detail
