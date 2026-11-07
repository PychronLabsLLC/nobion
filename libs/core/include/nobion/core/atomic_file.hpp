#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "nobion/core/error.hpp"

namespace nobion {

enum class Durability {
  Crash,  // survives a process crash: no sync
  Power   // survives power loss: file, rename and directory reach the disk
};

// Writes `bytes` to a temporary file beside `path` (same file system), flushes
// it, and renames it over `path`. Either the old content or the new is left at
// `path`, never a mix or nothing. A symlink at `path` is replaced by the new
// file, not followed. An existing file's permission bits are kept (POSIX).
//
// Power: POSIX syncs the file (macOS: F_FULLFSYNC, since fsync there does not
// flush the drive cache), renames, then syncs the directory. Windows:
// FlushFileBuffers, then MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH), retried
// 5 times 50 ms apart on a sharing violation, then Io.
//
// `new_file_mode`: the permission bits (masked to 0777) of a file made where
// none exists (stat fails, a dangling link included), set on the temp file
// before any bytes are written; an existing file keeps its own bits, and
// Windows ignores it.
//
// An Io error from the directory sync comes after the rename: the new content
// is already in place. Any earlier error leaves the old content and no temp.
[[nodiscard]] Result<void> write_file_atomic(const std::filesystem::path& path,
                                             std::string_view bytes,
                                             Durability durability = Durability::Power,
                                             std::optional<std::filesystem::perms> new_file_mode = std::nullopt);

namespace testing {
// Makes step `step` of write_file_atomic fail with an Io error; 0 turns it off.
// Steps: 1 create temp, 2 write, 3 sync file, 4 rename, 5 sync directory.
void fail_atomic_write_at(int step) noexcept;
}  // namespace testing

}  // namespace nobion
