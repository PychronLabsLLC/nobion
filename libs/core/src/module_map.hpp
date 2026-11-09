#pragma once

// The images loaded in this process (executable and shared libraries), kept
// for the crash handler (spec 2026-11-06-field-diagnostics, 3.2). A frame
// address in a crash file is useless without the load address of its image,
// and the handler may not ask the loader (no dladdr in a signal), so the table
// is filled outside the signal and only read inside it. POSIX only; not
// installed, not part of the public API.

#if !defined(_WIN32)

namespace nobion::detail {

class ModuleMap {
 public:
  // Starts the table. macOS: registers a dyld add-image callback, which is
  // called at once for every image already loaded and later for each new
  // one. Linux: names the main executable (readlink("/proc/self/exe"),
  // dl_iterate_phdr gives it an empty name), then refresh(). Idempotent.
  // Not from a signal handler.
  static void install() noexcept;

  // Linux: re-reads the loaded images (dl_iterate_phdr) when the loader's
  // count of added images moved, into the table the handler is not reading,
  // then makes that one current. macOS: nothing (dyld calls back). Called by
  // the log hub's flusher thread; does nothing before install(). Not from a
  // signal handler.
  static void refresh() noexcept;

  // Async-signal-safe: one line per image,
  // `module <path> 0x<load hex> 0x<size hex>`, then
  // `modules truncated: <n>` when more images were loaded than the table
  // holds (1024). Writes nothing before install().
  static void write_to(int fd) noexcept;
};

}  // namespace nobion::detail

#endif
