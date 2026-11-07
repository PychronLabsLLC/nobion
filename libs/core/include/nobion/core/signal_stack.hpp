#pragma once

namespace nobion {

// Gives the calling thread an alternate signal stack (sigaltstack, 64 KiB), so
// the crash handler (LogHub::install_crash_handlers, which sets SA_ONSTACK)
// still runs after a stack overflow on it. Called first thing in every thread
// entry we own. Idempotent: a thread that already has an alternate stack
// (its own, or one a sanitizer gave it) keeps it. The stack is allocated once
// per thread and freed when the thread exits. Does nothing on Windows.
void install_signal_stack() noexcept;

}  // namespace nobion
