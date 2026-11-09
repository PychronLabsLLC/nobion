#include "nobion/core/log_context.hpp"

#include "log_hub_internal.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#if !defined(_WIN32)
#include <pthread.h>
#endif

namespace nobion::detail {
namespace {

#if !defined(_WIN32)
// The same pointer as t_tag, kept under a pthread key for the crash handler:
// on macOS the first read of a thread_local on a thread that never touched
// one allocates (dyld's TLV path), while pthread_getspecific only reads the
// thread's key slot. Created once, outside any signal (first Thread scope,
// or LogHub::install_crash_handlers); the handler reads it only after
// g_tag_key_ready says it exists.
pthread_key_t g_tag_key{};
std::atomic<bool> g_tag_key_ready{false};
std::once_flag g_tag_key_once;
#endif

void set_tag_key(const std::string* tag) noexcept {
#if !defined(_WIN32)
  ensure_thread_tag_key();
  if (g_tag_key_ready.load(std::memory_order_acquire)) {
    (void)::pthread_setspecific(g_tag_key, tag);
  }
#else
  (void)tag;
#endif
}

}  // namespace
}  // namespace nobion::detail

namespace nobion::LogContext {
namespace {

// Read and written only by the owning thread. Pointers to the innermost open
// scope's string, so nothing here has a destructor: a thread that logs while
// it exits, after other thread_locals are gone, never reads a destroyed string.
thread_local const std::string* t_tag = nullptr;
thread_local const std::string* t_run = nullptr;
thread_local std::uint32_t t_number = 0;
thread_local int t_polling = 0;

static_assert(std::is_trivially_destructible_v<decltype(t_tag)>);
static_assert(std::is_trivially_destructible_v<decltype(t_run)>);
static_assert(std::is_trivially_destructible_v<decltype(t_number)>);
static_assert(std::is_trivially_destructible_v<decltype(t_polling)>);

std::atomic<std::uint32_t> g_next_number{1};

}  // namespace

Thread::Thread(std::string tag) : value_(std::move(tag)), previous_(std::exchange(t_tag, &value_)) {
  detail::set_tag_key(&value_);
}
Thread::~Thread() {
  t_tag = previous_;
  detail::set_tag_key(previous_);
}

Run::Run(std::string run_id) : value_(std::move(run_id)), previous_(std::exchange(t_run, &value_)) {}
Run::~Run() { t_run = previous_; }

Polling::Polling() noexcept { ++t_polling; }
Polling::~Polling() { --t_polling; }

bool polling() noexcept { return t_polling > 0; }

std::string_view thread_tag() noexcept { return t_tag != nullptr ? std::string_view{*t_tag} : std::string_view{}; }
std::string_view run_id() noexcept { return t_run != nullptr ? std::string_view{*t_run} : std::string_view{}; }

std::uint32_t thread_number() noexcept {
  if (t_number == 0) t_number = g_next_number.fetch_add(1, std::memory_order_relaxed);
  return t_number;
}

}  // namespace nobion::LogContext

namespace nobion::detail {

void ensure_thread_tag_key() noexcept {
#if !defined(_WIN32)
  try {
    std::call_once(g_tag_key_once, [] {
      if (::pthread_key_create(&g_tag_key, nullptr) == 0) g_tag_key_ready.store(true, std::memory_order_release);
    });
  } catch (...) {  // NOLINT(bugprone-empty-catch): no key; the crash handler writes `thread ?`
  }
#endif
}

const std::string* thread_tag_pointer() noexcept {
#if !defined(_WIN32)
  if (!g_tag_key_ready.load(std::memory_order_acquire)) return nullptr;
  return static_cast<const std::string*>(::pthread_getspecific(g_tag_key));
#else
  return LogContext::t_tag;
#endif
}

}  // namespace nobion::detail
