#include "module_map.hpp"

#if !defined(_WIN32)

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string_view>

#include <unistd.h>

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif

namespace nobion::detail {
namespace {

constexpr std::size_t kMaxModules = 512;
constexpr std::size_t kMaxPath = 512;  // longer paths are cut (still a usable hint)

struct Module {
  std::array<char, kMaxPath> path{};  // NUL-terminated
  std::uintptr_t load = 0;
  std::uintptr_t size = 0;
};

void copy_path(Module& m, const char* path) noexcept {
  const char* source = (path != nullptr && path[0] != '\0') ? path : "?";
  const std::size_t n = std::min(std::strlen(source), kMaxPath - 1);
  std::memcpy(m.path.data(), source, n);
  m.path[n] = '\0';
}

// write(2) of a whole buffer; errors ignored (nothing to report them to).
void write_all(int fd, const char* data, std::size_t size) noexcept {
  while (size > 0) {
    const ssize_t n = ::write(fd, data, size);
    if (n <= 0) return;
    data += n;
    size -= static_cast<std::size_t>(n);
  }
}

std::size_t put_hex(char* buf, std::size_t pos, std::uintptr_t value) noexcept {
  std::array<char, 2 * sizeof(std::uintptr_t)> digits{};
  std::size_t n = 0;
  do {
    digits[n++] = "0123456789abcdef"[value % 16];
    value /= 16;
  } while (value != 0 && n < digits.size());
  while (n > 0) buf[pos++] = digits[--n];
  return pos;
}

std::size_t put_text(char* buf, std::size_t pos, std::string_view text) noexcept {
  std::memcpy(buf + pos, text.data(), text.size());
  return pos + text.size();
}

// `module <path> 0x<load> 0x<size>\n`, formatted on the caller's stack.
void write_module(int fd, const Module& m) noexcept {
  std::array<char, kMaxPath + 64> line{};
  std::size_t pos = put_text(line.data(), 0, "module ");
  pos = put_text(line.data(), pos, std::string_view(m.path.data()));
  pos = put_text(line.data(), pos, " 0x");
  pos = put_hex(line.data(), pos, m.load);
  pos = put_text(line.data(), pos, " 0x");
  pos = put_hex(line.data(), pos, m.size);
  line[pos++] = '\n';
  write_all(fd, line.data(), pos);
}

void write_truncated(int fd, std::size_t truncated) noexcept {
  if (truncated == 0) return;
  std::array<char, 64> line{};
  std::size_t pos = put_text(line.data(), 0, "modules truncated: ");
  // Decimal.
  std::array<char, 24> digits{};
  std::size_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + truncated % 10);
    truncated /= 10;
  } while (truncated != 0 && n < digits.size());
  while (n > 0) line[pos++] = digits[--n];
  line[pos++] = '\n';
  write_all(fd, line.data(), pos);
}

std::atomic<bool> g_installed{false};

#if defined(__APPLE__)

// One table, appended to by dyld's callback: an entry below `g_count` is
// complete and never written again, so the handler reads a prefix nobody is
// writing. An image that is unloaded keeps its row (dlclose is rare here; a
// stale row only names an address range nothing runs in any more).
std::array<Module, kMaxModules> g_modules;
std::atomic<std::size_t> g_count{0};
std::atomic<std::size_t> g_truncated{0};
std::mutex g_add_mutex;  // dyld serialises its callbacks; this makes it certain

// Size of the image's __TEXT segment: what holds the code a frame points
// into. (The span of all segments is no use: images in the dyld shared cache
// share one __LINKEDIT far from their text.)
// NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast): a Mach-O header is walked by its load commands
std::uintptr_t text_size(const mach_header* header) noexcept {
  if (header->magic != MH_MAGIC_64) return 0;
  const auto* image = reinterpret_cast<const mach_header_64*>(header);
  const auto* cmd = reinterpret_cast<const load_command*>(image + 1);
  for (std::uint32_t i = 0; i < image->ncmds; ++i) {
    if (cmd->cmd == LC_SEGMENT_64) {
      const auto* segment = reinterpret_cast<const segment_command_64*>(cmd);
      if (std::strncmp(segment->segname, SEG_TEXT, sizeof segment->segname) == 0)
        return static_cast<std::uintptr_t>(segment->vmsize);
    }
    cmd = reinterpret_cast<const load_command*>(reinterpret_cast<const char*>(cmd) + cmd->cmdsize);
  }
  return 0;
}
// NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

void on_add_image(const mach_header* header, intptr_t /*slide*/) {
  const std::lock_guard lock(g_add_mutex);
  const std::size_t i = g_count.load(std::memory_order_relaxed);
  if (i >= kMaxModules) {
    g_truncated.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Module& m = g_modules[i];
  Dl_info info{};
  copy_path(m, ::dladdr(header, &info) != 0 ? info.dli_fname : nullptr);
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the address is what is written
  m.load = reinterpret_cast<std::uintptr_t>(header);  // what atos -l takes
  m.size = text_size(header);
  g_count.store(i + 1, std::memory_order_release);
}

#else

// Two tables: refresh() fills the one `g_current` does not name, then names
// it. The handler reads the current one, which nobody writes unless two
// refreshes (a second apart, from the flusher) both fall inside one crash.
struct Table {
  std::array<Module, kMaxModules> modules;
  std::size_t count = 0;
  std::size_t truncated = 0;
};
std::array<Table, 2> g_tables;
std::atomic<int> g_current{-1};  // -1: none yet
std::mutex g_refresh_mutex;
std::array<char, kMaxPath> g_exe{};  // the main executable's path, from /proc/self/exe
unsigned long long g_last_adds = 0;
bool g_have_adds = false;

int count_adds(dl_phdr_info* info, std::size_t size, void* data) {
  auto* adds = static_cast<unsigned long long*>(data);
  // Older loaders hand a shorter struct; then every refresh re-reads.
  if (size >= offsetof(dl_phdr_info, dlpi_adds) + sizeof info->dlpi_adds) {
    *adds = info->dlpi_adds;
  } else {
    *adds = ~0ULL;
  }
  return 1;  // the first image carries the counters; stop
}

int collect(dl_phdr_info* info, std::size_t /*size*/, void* data) {
  auto& table = *static_cast<Table*>(data);
  if (table.count >= kMaxModules) {
    ++table.truncated;
    return 0;
  }
  std::uintptr_t low = UINTPTR_MAX;
  std::uintptr_t high = 0;
  for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
    const auto& ph = info->dlpi_phdr[i];
    if (ph.p_type != PT_LOAD) continue;
    low = std::min<std::uintptr_t>(low, ph.p_vaddr);
    high = std::max<std::uintptr_t>(high, ph.p_vaddr + ph.p_memsz);
  }
  if (high == 0) return 0;  // nothing mapped
  Module& m = table.modules[table.count++];
  // The empty name is the main executable's (always the first image).
  const bool unnamed = info->dlpi_name == nullptr || info->dlpi_name[0] == '\0';
  copy_path(m, unnamed && table.count == 1 ? g_exe.data() : info->dlpi_name);
  // Address of the lowest PT_LOAD segment: the image's base for a PIE or a
  // shared library (p_vaddr 0), its fixed address for a non-PIE executable.
  m.load = static_cast<std::uintptr_t>(info->dlpi_addr) + low;
  m.size = high - low;
  return 0;
}

#endif

}  // namespace

void ModuleMap::install() noexcept {
  if (g_installed.exchange(true)) return;
#if defined(__APPLE__)
  _dyld_register_func_for_add_image(&on_add_image);
#else
  const ssize_t n = ::readlink("/proc/self/exe", g_exe.data(), g_exe.size() - 1);
  g_exe[n > 0 ? static_cast<std::size_t>(n) : 0] = '\0';
  refresh();
#endif
}

void ModuleMap::refresh() noexcept {
#if !defined(__APPLE__)
  if (!g_installed.load()) return;
  try {
    const std::lock_guard lock(g_refresh_mutex);
    unsigned long long adds = 0;
    ::dl_iterate_phdr(&count_adds, &adds);
    if (g_have_adds && adds == g_last_adds && adds != ~0ULL) return;
    const int current = g_current.load(std::memory_order_acquire);
    const int next = current == 0 ? 1 : 0;
    Table& table = g_tables[static_cast<std::size_t>(next)];
    table.count = 0;
    table.truncated = 0;
    ::dl_iterate_phdr(&collect, &table);
    g_current.store(next, std::memory_order_release);
    g_last_adds = adds;
    g_have_adds = true;
  } catch (...) {  // NOLINT(bugprone-empty-catch): a mutex that cannot be locked; the old table stays
  }
#endif
}

void ModuleMap::write_to(int fd) noexcept {
  if (fd < 0) return;
#if defined(__APPLE__)
  const std::size_t count = g_count.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < count; ++i) write_module(fd, g_modules[i]);
  write_truncated(fd, g_truncated.load(std::memory_order_relaxed));
#else
  const int current = g_current.load(std::memory_order_acquire);
  if (current < 0) return;
  const Table& table = g_tables[static_cast<std::size_t>(current)];
  for (std::size_t i = 0; i < table.count; ++i) write_module(fd, table.modules[i]);
  write_truncated(fd, table.truncated);
#endif
}

}  // namespace nobion::detail

#endif
