#include "nobion/core/signal_stack.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <csignal>
#include <cstddef>

#include <sys/mman.h>
#endif

namespace nobion {

#if defined(_WIN32)
namespace {

// What a stack overflow leaves the exception filter (log_hub.cpp).
constexpr ULONG kStackGuarantee = 64 * 1024;

}  // namespace
#else
namespace {

constexpr std::size_t kSignalStackSize = 64 * 1024;

// The calling thread's alternate stack, owned by a thread_local: freed when
// the thread exits, after it has been switched off.
class SignalStack {
 public:
  SignalStack() = default;
  ~SignalStack() {
    if (memory_ == nullptr) return;
    stack_t current{};
    if (::sigaltstack(nullptr, &current) == 0 && current.ss_sp == memory_) {
      stack_t off{};
      off.ss_flags = SS_DISABLE;
      ::sigaltstack(&off, nullptr);
    }
    ::munmap(memory_, kSignalStackSize);
  }
  SignalStack(const SignalStack&) = delete;
  SignalStack& operator=(const SignalStack&) = delete;
  SignalStack(SignalStack&&) = delete;
  SignalStack& operator=(SignalStack&&) = delete;

  void install() noexcept {
    stack_t current{};
    if (::sigaltstack(nullptr, &current) != 0) return;
    // Ours already, or one someone else gave this thread (a sanitizer's): kept.
    if ((current.ss_flags & SS_DISABLE) == 0) return;
    // mmap, not malloc: the stack is never in the heap a crash may have broken.
    void* memory = ::mmap(nullptr, kSignalStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (memory == MAP_FAILED) return;
    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = kSignalStackSize;
    stack.ss_flags = 0;
    if (::sigaltstack(&stack, nullptr) != 0) {
      ::munmap(memory, kSignalStackSize);
      return;
    }
    memory_ = memory;
  }

 private:
  void* memory_ = nullptr;
};

thread_local SignalStack t_signal_stack;

}  // namespace
#endif

void install_signal_stack() noexcept {
#if defined(_WIN32)
  // Setting the same size again is accepted; a failure (a larger guarantee
  // already set) leaves the thread as it was.
  ULONG guarantee = kStackGuarantee;
  (void)SetThreadStackGuarantee(&guarantee);
#else
  t_signal_stack.install();
#endif
}

}  // namespace nobion
