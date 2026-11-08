#pragma once

// Reads a zip back for a test: every entry, name -> bytes. Framework-free so
// a GoogleTest suite and a Qt Test suite can both use it; the target links
// miniz::miniz.

#include <miniz.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>

namespace nobion::testing {

// The entries of the zip at `path`; nullopt when it cannot be opened or an
// entry cannot be read.
inline std::optional<std::map<std::string, std::string>> zip_entries(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) return std::nullopt;
  std::map<std::string, std::string> out;
  bool ok = true;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count && ok; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
      ok = false;
      break;
    }
    std::string data(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
    if (!data.empty() && !mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0)) ok = false;
    out.emplace(stat.m_filename, std::move(data));
  }
  mz_zip_reader_end(&zip);
  if (!ok) return std::nullopt;
  return out;
}

}  // namespace nobion::testing
