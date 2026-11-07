#include "nobion/core/atomic_file.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <system_error>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nobion {
namespace {

std::atomic<int> g_fail_at{0};
std::atomic<unsigned> g_counter{0};

bool injected(int step) noexcept { return g_fail_at.load(std::memory_order_relaxed) == step; }

Unexpected<Error> io(const std::string& what, const std::filesystem::path& path, int code) {
  return fail(ErrorKind::Io, what + " " + path.string() + ": " + std::generic_category().message(code));
}

Unexpected<Error> injected_failure(int step, const std::filesystem::path& path) {
  return fail(ErrorKind::Io, "injected failure at step " + std::to_string(step) + " for " +
                                 path.string());
}

std::filesystem::path temp_path_for(const std::filesystem::path& path) {
#ifdef _WIN32
  const auto pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
  const auto pid = static_cast<unsigned long>(::getpid());
#endif
  const auto n = g_counter.fetch_add(1, std::memory_order_relaxed);
  // operator+= keeps the native (wide, on Windows) string: no narrowing.
  auto tmp = path;
  tmp += ".tmp." + std::to_string(pid) + "." + std::to_string(n);
  return tmp;
}

#ifdef _WIN32

std::string win_message(DWORD code) { return "Windows error " + std::to_string(code); }

Unexpected<Error> win_io(const std::string& what, const std::filesystem::path& path, DWORD code) {
  return fail(ErrorKind::Io, what + " " + path.string() + ": " + win_message(code));
}

Result<void> write_windows(const std::filesystem::path& path, std::string_view bytes,
                           Durability durability) {
  const auto tmp = temp_path_for(path);
  if (injected(1)) {
    return injected_failure(1, path);
  }
  HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    return win_io("cannot create", tmp, ::GetLastError());
  }
  const auto discard = [&](Unexpected<Error> e) {
    ::CloseHandle(h);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return Result<void>(std::move(e));
  };
  if (injected(2)) {
    return discard(injected_failure(2, path));
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - done, 1U << 30));
    DWORD wrote = 0;
    if (::WriteFile(h, bytes.data() + done, chunk, &wrote, nullptr) == 0) {
      return discard(win_io("cannot write", tmp, ::GetLastError()));
    }
    done += wrote;
  }
  if (durability == Durability::Power) {
    if (injected(3) || ::FlushFileBuffers(h) == 0) {
      return discard(injected(3) ? injected_failure(3, path)
                                 : win_io("cannot flush", tmp, ::GetLastError()));
    }
  }
  ::CloseHandle(h);
  h = INVALID_HANDLE_VALUE;
  const DWORD flags =
      MOVEFILE_REPLACE_EXISTING | (durability == Durability::Power ? MOVEFILE_WRITE_THROUGH : 0);
  DWORD last = 0;
  bool moved = false;
  for (int attempt = 0; attempt < 5 && !moved; ++attempt) {
    if (attempt > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (injected(4)) {
      last = ERROR_GEN_FAILURE;
      break;
    }
    if (::MoveFileExW(tmp.c_str(), path.c_str(), flags) != 0) {
      moved = true;
    } else {
      last = ::GetLastError();
      if (last != ERROR_SHARING_VIOLATION && last != ERROR_ACCESS_DENIED) {
        break;
      }
    }
  }
  if (!moved) {
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    return injected(4) ? Result<void>(injected_failure(4, path))
                       : Result<void>(win_io("cannot replace", path, last));
  }
  // MOVEFILE_WRITE_THROUGH covers the directory entry: no separate step.
  if (injected(5)) {
    return injected_failure(5, path);
  }
  return {};
}

#else

// Flushes a file's data to the device; on macOS past the drive cache.
int sync_fd(int fd) {
#ifdef __APPLE__
  if (::fcntl(fd, F_FULLFSYNC) == 0) {
    return 0;
  }
  if (errno != ENOTSUP && errno != EINVAL) {
    return -1;
  }
#endif
  return ::fsync(fd);
}

void close_quietly(int fd) {
  const int saved = errno;
  ::close(fd);
  errno = saved;
}

Result<void> write_posix(const std::filesystem::path& path, std::string_view bytes,
                         Durability durability) {
  const auto tmp = temp_path_for(path);
  const auto discard = [&](Unexpected<Error> e) {
    ::unlink(tmp.c_str());
    return Result<void>(std::move(e));
  };
  if (injected(1)) {
    return injected_failure(1, path);
  }
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd < 0) {
    return io("cannot create", tmp, errno);
  }
  if (injected(2)) {
    ::close(fd);
    return discard(injected_failure(2, path));
  }
  // Replacing a file keeps its permission bits (a 0600 file stays 0600).
  struct stat old_st {};
  if (::stat(path.c_str(), &old_st) == 0) {
    (void)::fchmod(fd, old_st.st_mode & 07777);
  }
  std::size_t done = 0;
  while (done < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + done, bytes.size() - done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      close_quietly(fd);
      return discard(io("cannot write", tmp, errno));
    }
    if (n == 0) {
      ::close(fd);
      return discard(fail(ErrorKind::Io, "cannot write " + tmp.string() + ": wrote no bytes"));
    }
    done += static_cast<std::size_t>(n);
  }
  if (durability == Durability::Power) {
    if (injected(3)) {
      ::close(fd);
      return discard(injected_failure(3, path));
    }
    if (sync_fd(fd) != 0) {
      close_quietly(fd);
      return discard(io("cannot sync", tmp, errno));
    }
  }
  if (::close(fd) != 0) {
    return discard(io("cannot close", tmp, errno));
  }
  if (injected(4)) {
    return discard(injected_failure(4, path));
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    return discard(io("cannot replace", path, errno));
  }
  if (durability == Durability::Power) {
    if (injected(5)) {
      return injected_failure(5, path);
    }
    const auto dir = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    const int dfd = ::open(dir.c_str(), O_RDONLY | O_CLOEXEC);
    if (dfd < 0) {
      return io("cannot open directory", dir, errno);
    }
    if (::fsync(dfd) != 0) {
      close_quietly(dfd);
      return io("cannot sync directory", dir, errno);
    }
    ::close(dfd);
  }
  return {};
}

#endif

}  // namespace

Result<void> write_file_atomic(const std::filesystem::path& path, std::string_view bytes,
                               Durability durability) {
#ifdef _WIN32
  return write_windows(path, bytes, durability);
#else
  return write_posix(path, bytes, durability);
#endif
}

namespace testing {
void fail_atomic_write_at(int step) noexcept { g_fail_at.store(step, std::memory_order_relaxed); }
}  // namespace testing

}  // namespace nobion
