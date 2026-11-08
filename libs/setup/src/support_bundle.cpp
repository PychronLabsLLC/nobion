#include "nobion/setup/support_bundle.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "nobion/core/atomic_file.hpp"
#include "nobion/core/build_info.hpp"
#include "nobion/core/clock.hpp"
#include "nobion/core/host_name.hpp"
#include "nobion/core/log_dir.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/core/redact.hpp"
#include "nobion/core/user_file.hpp"
#include "nobion/setup/install.hpp"
#include "nobion/setup/installer.hpp"

#if !defined(_WIN32)
#include <sys/utsname.h>
#endif

namespace nobion::setup {

namespace {

namespace fs = std::filesystem;
using SysClock = std::chrono::system_clock;

// A config file larger than this is left out (a database or an image that
// happens to sit in the root is no config).
constexpr std::uintmax_t kMaxConfigFile = 16u << 20;
// What miniz writes per entry besides its data and twice its name: local
// header (30), data descriptor (16), central header (46), with room for a
// zip64 extra field.
constexpr std::uint64_t kEntryOverhead = 30 + 16 + 46 + 64;
constexpr std::uint64_t kEndOverhead = 22 + 76;  // end of central directory (+ zip64 records)

// The config files copied: these extensions only.
constexpr std::array<std::string_view, 7> kConfigExtensions = {".toml", ".txt", ".json", ".yaml", ".yml", ".csv", ".md"};

enum class Redact { Text, Toml, None };

struct Entry {
  std::string name;               // zip path, UTF-8
  std::string payload;            // the bytes stored: deflated, or as is
  std::uint64_t raw_size = 0;
  mz_uint32 crc = 0;
  bool deflated = false;
  std::time_t mtime = 0;
  bool droppable = false;         // may go to keep under max_bytes
  [[nodiscard]] std::uint64_t cost() const { return payload.size() + 2 * name.size() + kEntryOverhead; }
};

// A path as UTF-8 with '/' separators, whatever the platform's narrow
// encoding (path::string() throws on Windows for a name outside the code page).
std::string utf8(const fs::path& p) {
  const std::u8string u = p.generic_u8string();
  return {u.begin(), u.end()};
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

fs::file_type own_type(const fs::path& p) {
  std::error_code ec;
  const auto st = fs::symlink_status(p, ec);  // the entry itself: a link is never followed
  return ec ? fs::file_type::not_found : st.type();
}
bool is_link(const fs::path& p) { return own_type(p) == fs::file_type::symlink; }
bool is_plain_file(const fs::path& p) { return own_type(p) == fs::file_type::regular; }
bool is_plain_dir(const fs::path& p) { return own_type(p) == fs::file_type::directory; }

// A name that says it holds a secret (credentials.local.toml, my_secrets.toml).
bool secret_name(const fs::path& p) {
  const std::string name = lower(utf8(p.filename()));
  return name.find("credential") != std::string::npos || name.find("secret") != std::string::npos;
}

bool hidden(const fs::path& rel) {
  return std::any_of(rel.begin(), rel.end(), [](const fs::path& part) {
    const std::string s = utf8(part);
    return !s.empty() && s.front() == '.' && s != "." && s != "..";
  });
}

std::time_t to_time_t(fs::file_time_type t) {
  const auto sys = SysClock::now() + std::chrono::duration_cast<SysClock::duration>(t - fs::file_time_type::clock::now());
  return SysClock::to_time_t(sys);
}

std::time_t mtime_of(const fs::path& p, std::time_t fallback) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  return ec ? fallback : to_time_t(t);
}

std::string utc(std::time_t t, const char* format) {
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  gmtime_r(&t, &tm);
#endif
  std::array<char, 32> buffer{};
  const std::size_t n = std::strftime(buffer.data(), buffer.size(), format, &tm);
  return {buffer.data(), n};
}

struct FileRead {
  std::string bytes;
  bool cut = false;
};

// The file's bytes, or its last `cap` bytes when it is larger (from the next
// line start when `text`), read without loading the rest. Nothing for a link,
// a missing file or one that cannot be read.
std::optional<FileRead> read_capped(const fs::path& path, std::uint64_t cap, bool text) {
  if (!is_plain_file(path)) return std::nullopt;
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return std::nullopt;
  const std::streamoff size = in.tellg();
  if (size < 0) return std::nullopt;
  FileRead r;
  auto take = static_cast<std::uint64_t>(size);
  if (take > cap) {
    take = cap;
    r.cut = true;
  }
  in.seekg(static_cast<std::streamoff>(static_cast<std::uint64_t>(size) - take));
  r.bytes.resize(static_cast<std::size_t>(take));
  if (take > 0 && !in.read(r.bytes.data(), static_cast<std::streamsize>(take))) return std::nullopt;
  if (r.cut && text) {
    const auto nl = r.bytes.find('\n');
    r.bytes.erase(0, nl == std::string::npos ? r.bytes.size() : nl + 1);
  }
  return r;
}

std::string redacted(std::string_view bytes, Redact how) {
  switch (how) {
    case Redact::Toml: return redact::in_toml(bytes);
    case Redact::Text: return redact::in_text(bytes);
    case Redact::None: break;
  }
  return std::string(bytes);
}

Redact redaction_for(const fs::path& path) {
  const auto ext = path.extension();
  if (ext == ".toml") return Redact::Toml;
  if (ext == ".dmp") return Redact::None;  // a minidump: binary, kept as written
  return Redact::Text;
}

// Deflates `data` (raw deflate, as a zip stores it) unless that does not make it smaller.
Entry make_entry(std::string name, std::string data, std::time_t mtime, bool droppable) {
  Entry e;
  e.name = std::move(name);
  e.mtime = mtime;
  e.droppable = droppable;
  e.raw_size = data.size();
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): miniz takes bytes as unsigned char
  e.crc = static_cast<mz_uint32>(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const unsigned char*>(data.data()), data.size()));
  if (!data.empty()) {
    std::size_t out_len = 0;
    const int flags = static_cast<int>(tdefl_create_comp_flags_from_zip_params(MZ_DEFAULT_LEVEL, -MZ_DEFAULT_WINDOW_BITS,
                                                                               MZ_DEFAULT_STRATEGY));
    void* packed = tdefl_compress_mem_to_heap(data.data(), data.size(), &out_len, flags);
    if (packed != nullptr && out_len < data.size()) {
      e.payload.assign(static_cast<const char*>(packed), out_len);
      e.deflated = true;
    }
    mz_free(packed);
  }
  if (!e.deflated) e.payload = std::move(data);
  return e;
}

struct Dropped {
  std::time_t mtime;
  std::string name;
};

struct Collector {
  std::uint64_t cap;                  // max_bytes: also the most one file gives
  std::vector<Entry> entries;
  std::set<std::string> names;
  std::uint64_t total = kEndOverhead;  // estimated zip size of `entries`
  std::vector<std::string> left_out;  // "zip path: why"
  std::vector<std::string> cut;       // zip paths of files of which only the tail went in
  std::vector<Dropped> dropped;
  std::size_t config_left_out = 0;
  std::size_t links_skipped = 0;
  std::time_t now = SysClock::to_time_t(SysClock::now());  // real time: file times are compared with it

  explicit Collector(std::uint64_t max_bytes) : cap(max_bytes) {}

  bool add_text(const std::string& name, std::string_view text, Redact how, bool droppable = false,
                std::optional<std::time_t> mtime = std::nullopt) {
    if (!names.insert(name).second) {
      left_out.push_back(name + ": a second file of that name");
      return false;
    }
    entries.push_back(make_entry(name, redacted(text, how), mtime.value_or(now), droppable));
    total += entries.back().cost();
    return true;
  }

  // Reads `path` (its tail when larger than the cap) and adds it under
  // `name`; a link is skipped, a file that cannot be read is named in README.
  // A TOML file over the cap is left out whole: a tail can begin inside a
  // multi-line secret, which the key rule would then not see.
  bool add_file(const std::string& name, const fs::path& path, bool droppable) {
    if (is_link(path)) {
      ++links_skipped;
      return false;
    }
    const Redact how = redaction_for(path);
    if (how == Redact::Toml) {
      std::error_code ec;
      if (const auto size = fs::file_size(path, ec); !ec && size > cap) {
        left_out.push_back(name + ": larger than the bundle's cap");
        return false;
      }
    }
    auto read = read_capped(path, cap, how != Redact::None);
    if (!read) {
      left_out.push_back(name + ": cannot be read");
      return false;
    }
    if (read->cut) cut.push_back(name);
    return add_text(name, read->bytes, how, droppable, mtime_of(path, now));
  }
};

std::string json_escape(std::string_view s) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          const auto byte = static_cast<unsigned char>(c);
          out += "\\u00";
          out += kHex[byte >> 4U];
          out += kHex[byte & 0xFU];
        } else {
          out += c;
        }
    }
  }
  return out;
}

// `"text"`, escaped for JSON.
std::string json_string(std::string_view text) { return '"' + json_escape(text) + '"'; }

struct HostFacts {
  std::string os, machine;
};

HostFacts host_facts() {
#if defined(_WIN32)
  return {"Windows", ""};
#else
  utsname u{};
  if (::uname(&u) != 0) return {"unknown", ""};
  return {std::string(u.sysname) + " " + u.release, u.machine};
#endif
}

// `true` when `p` is `dir` or inside it (both made absolute and normal).
bool is_under(const fs::path& p, const fs::path& dir) {
  if (dir.empty()) return false;
  std::error_code ec;
  const auto a = fs::absolute(p, ec).lexically_normal();
  if (ec) return false;
  const auto b = fs::absolute(dir, ec).lexically_normal();
  if (ec) return false;
  auto ai = a.begin();
  for (auto bi = b.begin(); bi != b.end(); ++bi, ++ai) {
    if (bi->empty()) continue;  // a trailing separator
    if (ai == a.end() || *ai != *bi) return false;
  }
  return true;
}

bool ends_with(std::string_view s, std::string_view suffix) {
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// spdlog names rotated files `<stem>.<n>.log`.
bool is_rotated_log(const fs::path& p) {
  const std::string stem = utf8(p.stem());
  const auto dot = stem.rfind('.');
  if (dot == std::string::npos || dot + 1 == stem.size()) return false;
  return std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(dot) + 1, stem.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

// The plain files of `dir` (not a link itself), links among them counted and skipped.
std::vector<fs::path> plain_files(Collector& c, const fs::path& dir) {
  std::vector<fs::path> out;
  if (dir.empty()) return out;
  if (is_link(dir)) {
    ++c.links_skipped;
    return out;
  }
  if (!is_plain_dir(dir)) return out;
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fe;
    if (it->is_symlink(fe)) {
      ++c.links_skipped;
    } else if (it->is_regular_file(fe)) {
      out.push_back(it->path());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

// A file that may be dropped to keep under the cap, not read yet.
struct Candidate {
  fs::path path;
  std::string name;
  std::time_t mtime;
};

std::vector<Candidate> log_candidates(Collector& c, const fs::path& log_dir, const BundleOptions& options) {
  // logs_since is measured on real time, as file times are.
  const std::time_t oldest = c.now - std::chrono::duration_cast<std::chrono::seconds>(options.logs_since).count();
  std::vector<Candidate> out;
  for (const auto& p : plain_files(c, log_dir)) {
    if (p.extension() != ".log" || secret_name(p)) continue;
    const std::string name = "logs/" + utf8(p.filename());
    const std::time_t mtime = mtime_of(p, c.now);
    if (is_rotated_log(p) && mtime < oldest) {
      c.left_out.push_back(name + ": older than " + std::to_string(options.logs_since.count()) + " h");
      continue;
    }
    out.push_back({p, name, mtime});
  }
  return out;
}

std::vector<Candidate> other_candidates(Collector& c, const fs::path& log_dir, const fs::path& trace_dir,
                                        const BundleOptions& options, const std::optional<fs::path>& fresh_dump) {
  std::vector<Candidate> out;
  const auto add_dir = [&](const fs::path& dir, const std::string& prefix, std::string_view ext) {
    for (const auto& p : plain_files(c, dir)) {
      if (secret_name(p) || (!ext.empty() && p.extension() != ext)) continue;
      out.push_back({p, prefix + utf8(p.filename()), mtime_of(p, c.now)});
    }
  };
  if (!log_dir.empty()) {
    add_dir(log_dir / "crash", "logs/crash/", {});
    add_dir(log_dir / "flight", "logs/flight/", {});
  }
  if (fresh_dump && !is_under(*fresh_dump, log_dir / "flight")) {
    out.push_back({*fresh_dump, "logs/flight/" + utf8(fresh_dump->filename()), mtime_of(*fresh_dump, c.now)});
  }
  if (options.include_traces) {
    add_dir(trace_dir, "traces/", ".trace");
  } else {
    c.left_out.emplace_back("traces/: not asked for");
  }
  return out;
}

// Logs, crash files, dumps and traces, newest first: the newest log that can
// be read always goes in; the rest while the bundle is under the cap. What
// comes after the cap is reached is dropped without being read, so memory
// stays near the cap.
void collect_droppable(Collector& c, std::vector<Candidate> logs, std::vector<Candidate> others) {
  const auto newest_first = [](const Candidate& a, const Candidate& b) { return a.mtime > b.mtime; };
  std::stable_sort(logs.begin(), logs.end(), newest_first);
  std::vector<Candidate> rest;
  bool have_newest = false;
  for (auto& l : logs) {
    if (!have_newest) {
      have_newest = c.add_file(l.name, l.path, false);
    } else {
      rest.push_back(std::move(l));
    }
  }
  rest.insert(rest.end(), std::make_move_iterator(others.begin()), std::make_move_iterator(others.end()));
  std::stable_sort(rest.begin(), rest.end(), newest_first);
  for (const auto& r : rest) {
    if (c.total >= c.cap) {
      c.dropped.push_back({r.mtime, r.name});
    } else {
      (void)c.add_file(r.name, r.path, true);
    }
  }
}

// A listing of a folder: relative name, size, age; never a body.
std::string listing(const fs::path& dir, std::time_t now) {
  if (!is_plain_dir(dir)) return "no folder " + utf8(dir.filename()) + "\n";
  std::vector<std::string> lines;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code fe;
    if (it->is_symlink(fe) || !it->is_regular_file(fe)) continue;
    const auto size = it->file_size(fe);
    const auto t = it->last_write_time(fe);
    const auto age = fe ? 0 : now - to_time_t(t);  // real time, as file times are
    lines.push_back(utf8(it->path().lexically_relative(dir)) + "\t" + std::to_string(size) + " bytes\t" +
                    std::to_string(age) + " s old");
  }
  std::sort(lines.begin(), lines.end());
  std::string out = utf8(dir.filename()) + ": " + std::to_string(lines.size()) + " files\n";
  for (const auto& l : lines) out += l + "\n";
  return out;
}

struct ConfigScan {
  std::vector<fs::path> config;  // relative to root
  std::vector<fs::path> state;   // *.state.toml, relative to root
};

bool allowed_config(const fs::path& p) {
  const std::string ext = lower(utf8(p.extension()));
  return std::find(kConfigExtensions.begin(), kConfigExtensions.end(), ext) != kConfigExtensions.end();
}

ConfigScan scan_root(Collector& c, const fs::path& root, const std::vector<fs::path>& skip_dirs) {
  ConfigScan scan;
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
    const fs::path& p = it->path();
    const fs::path rel = p.lexically_relative(root);
    std::error_code fe;
    if (it->is_symlink(fe)) {
      ++c.links_skipped;
      continue;  // a linked folder is not entered either (the iterator never follows one)
    }
    if (it->is_directory(fe)) {
      // Hidden folders (.nobion holds the credentials; .git), and folders
      // that are data, logs, traces or field tables (state/ takes those).
      const bool skip = hidden(rel) || std::any_of(skip_dirs.begin(), skip_dirs.end(),
                                                   [&](const fs::path& d) { return is_under(p, d); });
      if (skip) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(fe)) continue;
    if (secret_name(p) || hidden(rel)) {
      ++c.config_left_out;
      continue;
    }
    if (ends_with(utf8(p.filename()), ".state.toml")) {
      scan.state.push_back(rel);
      continue;
    }
    if (!allowed_config(p)) {
      ++c.config_left_out;
      continue;
    }
    if (const auto size = it->file_size(fe); !fe && size > kMaxConfigFile) {
      ++c.config_left_out;
      continue;
    }
    scan.config.push_back(rel);
  }
  std::sort(scan.config.begin(), scan.config.end());
  std::sort(scan.state.begin(), scan.state.end());
  return scan;
}

void collect_install(Collector& c, const SiteInstall& install, const fs::path& log_dir, const fs::path& trace_dir) {
  const fs::path& root = install.root;
  // An install that names no data folder keeps it in <root>/data (outbox bodies, the store).
  const fs::path data = install.data.empty() ? root / "data" : install.path(install.data);
  const fs::path tables = root / "tables";
  const auto scan = scan_root(c, root, {data, log_dir, trace_dir, tables});

  std::set<std::string> copied;
  for (const auto& rel : scan.config) {
    if (c.add_file("config/" + utf8(rel), root / rel, false)) copied.insert(utf8(rel));
  }

  // install.toml and what changed since the install wrote it.
  (void)c.add_file("config/install.toml", record_path(root), false);
  std::string edited;
  if (auto record = read_install_record(root)) {
    std::set<std::string> seen;
    for (const auto& [rel, sha] : record->files) {
      seen.insert(rel);
      const fs::path file = root / fs::path(std::u8string(rel.begin(), rel.end()));
      if (secret_name(file) || hidden(fs::path(rel))) continue;
      if (is_link(file)) continue;
      std::ifstream in(file, std::ios::binary);
      if (!in || !is_plain_file(file)) {
        edited += "missing " + rel + "\n";
        continue;
      }
      const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      if (sha256_hex(text) != sha) edited += "edited " + rel + "\n";
    }
    for (const auto& rel : copied) {
      if (!seen.contains(rel)) edited += "unknown " + rel + "\n";
    }
    if (edited.empty()) edited = "no file changed since the install\n";
  } else {
    edited = "no install record: " + record.error().what + "\n";
  }
  c.add_text("config/EDITED.txt", edited, Redact::Text);

  // Runtime state.
  for (const auto& rel : scan.state) (void)c.add_file("state/" + utf8(rel), root / rel, false);
  if (is_plain_dir(data) && is_plain_file(data / "executor_state.json")) {
    (void)c.add_file("state/executor_state.json", data / "executor_state.json", false);
  }
  if (is_plain_dir(tables)) {
    std::error_code ec;
    for (fs::directory_iterator t(tables, ec), end; !ec && t != end; t.increment(ec)) {
      if (!is_plain_dir(t->path())) continue;
      const std::string table = utf8(t->path().filename());
      const fs::path current = t->path() / "current";
      auto pointer = read_capped(current, c.cap, true);
      if (!pointer) continue;
      c.add_text("state/tables/" + table + "/current", pointer->bytes, Redact::Text);
      std::string version = pointer->bytes;
      while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back())) != 0) version.pop_back();
      // A name in the table's folder, never a path out of it.
      const fs::path named = fs::path(std::u8string(version.begin(), version.end())).filename();
      if (!named.empty() && !secret_name(named) && is_plain_file(t->path() / named)) {
        (void)c.add_file("state/tables/" + table + "/" + utf8(named), t->path() / named, false);
      }
    }
  }
  c.add_text("state/outbox.txt", listing(data / "outbox", c.now), Redact::Text);
  c.add_text("state/spool.txt", listing(data / "spool", c.now), Redact::Text);
}

std::string doctor_text(const SiteInstall* install, const BundleOptions& options) {
  if (install == nullptr) return "no install\n";
  DoctorOptions o = options.doctor;
  o.probe = false;  // never touches hardware
  std::string out;
  for (const auto& check : doctor(*install, o)) {
    out += '[';
    out += to_string(check.status);
    out += "] " + check.name;
    if (!check.detail.empty()) out += ": " + check.detail;
    if (!check.hint.empty()) out += " (hint: " + check.hint + ")";
    out += '\n';
  }
  return out;
}

std::string build_json(const SiteInstall* install, const BundleOptions& options, const fs::path& site_path,
                       const HostFacts& host, std::time_t stamp) {
  const BuildInfo& b = build_info();
  std::ostringstream j;
  j << "{\n";
  const auto field = [&j](std::string_view name, const std::string& json_value) {
    j << "  " << json_string(name) << ": " << json_value << ",\n";
  };
  field("build_id", json_string(build_id()));
  field("version", json_string(b.version));
  field("git_sha", json_string(b.git_sha));
  field("git_dirty", b.git_dirty ? "true" : "false");
  field("build_type", json_string(b.build_type));
  std::string features = "[";
  for (std::size_t i = 0; i < b.features.size(); ++i) features += (i ? ", " : "") + json_string(b.features[i]);
  field("features", features + "]");
  field("written", json_string(utc(stamp, "%Y-%m-%dT%H:%M:%SZ")));
  field("os", json_string(host.os));
  field("cpu", "{" + json_string("machine") + ": " + json_string(host.machine) + ", " + json_string("threads") + ": " +
                   std::to_string(std::thread::hardware_concurrency()) + "}");
  if (!options.qt_version.empty()) field("qt_version", json_string(options.qt_version));
  if (install != nullptr) {
    field("install", "{" + json_string("name") + ": " + json_string(install->name) + ", " + json_string("profile") +
                         ": " + json_string(install->profile) + ", " + json_string("root") + ": " +
                         json_string(utf8(install->root)) + "}");
  }
  j << "  " << json_string("site_toml") << ": " << json_string(utf8(site_path)) << "\n}\n";
  return j.str();
}

struct ReadmeFacts {
  const SiteInstall* install = nullptr;
  HostFacts host;
  std::time_t stamp = 0;
  std::uint64_t max_bytes = 0;
};

std::string readme(const ReadmeFacts& f, const Collector& c, const std::vector<std::string>& notes) {
  std::ostringstream r;
  r << "nobion support bundle\n"
    << "=====================\n\n"
    << "What a lab sends back when something goes wrong: logs, crash files, flight\n"
    << "recorder dumps, the install's config and state, wire traces and the doctor\n"
    << "report. In TOML files the values of password, token, secret and key entries\n"
    << "are masked as " << redact::kMask << "; everywhere the password of a URL is. Logs, crash files,\n"
    << "dumps and traces are masked for URL passwords only: read them before sending.\n"
    << "The credentials file is never included. Nothing was uploaded.\n\n"
    << "build    " << build_id() << "\n"
    << "host     " << host_name() << "\n"
    << "os       " << f.host.os << (f.host.machine.empty() ? "" : " " + f.host.machine) << "\n"
    << "written  " << utc(f.stamp, "%Y-%m-%dT%H:%M:%SZ") << "\n"
    << "install  " << (f.install ? f.install->name + " (" + utf8(f.install->root) + ")" : "none") << "\n\n";
  for (const auto& n : notes) r << n << "\n";
  if (!notes.empty()) r << "\n";
  r << "Left out\n--------\n";
  r << ".nobion/credentials.toml: never included\n";
  if (c.config_left_out > 0) {
    r << c.config_left_out
      << " config files left out (not .toml .txt .json .yaml .yml .csv .md, hidden, named like a secret, or over 16 "
         "MiB)\n";
  }
  if (c.links_skipped > 0) r << c.links_skipped << " symbolic links skipped (never followed)\n";
  for (const auto& l : c.left_out) r << l << "\n";
  for (const auto& name : c.cut) r << "cut " << name << ": only its last " << f.max_bytes << " bytes\n";
  if (!c.dropped.empty()) {
    std::vector<Dropped> dropped = c.dropped;
    std::stable_sort(dropped.begin(), dropped.end(),
                     [](const Dropped& a, const Dropped& b) { return a.mtime < b.mtime; });
    r << "\nDropped to keep the bundle under " << f.max_bytes << " bytes (oldest first)\n";
    for (const auto& d : dropped) r << "dropped " << d.name << "\n";
  }
  r << "\nA hung process\n--------------\n"
    << "The stacks of a process that hangs cannot be written from inside it. Take them\n"
    << "with (pid from the Activity Monitor, ps or Task Manager):\n"
    << "  macOS:   sample <pid> 10\n"
    << R"(  Linux:   gdb -p <pid> -batch -ex "thread apply all bt"   or   eu-stack -p <pid>)" << '\n'
    << "  Windows: procdump -ma <pid>\n";
  return r.str();
}

// Zips `entries` in memory and writes the archive to `out` (temporary file,
// then rename). Names are UTF-8: miniz flags each one so (general-purpose
// bit 11) unless told the name is ASCII.
Result<void> write_zip(const std::vector<Entry>& entries, const fs::path& out) {
  mz_zip_archive writer{};
  if (!mz_zip_writer_init_heap(&writer, 0, 1u << 20)) return fail(ErrorKind::Io, "cannot start the bundle archive");
  struct End {
    mz_zip_archive* zip;
    ~End() { mz_zip_writer_end(zip); }
  } end{&writer};
  for (const auto& e : entries) {
    MZ_TIME_T stamp = static_cast<MZ_TIME_T>(e.mtime);
    const mz_uint flags = e.deflated ? static_cast<mz_uint>(MZ_ZIP_FLAG_COMPRESSED_DATA) : mz_uint{0};
    if (!mz_zip_writer_add_mem_ex_v2(&writer, e.name.c_str(), e.payload.data(), e.payload.size(), nullptr, 0, flags,
                                     e.deflated ? e.raw_size : 0, e.deflated ? e.crc : 0, &stamp, nullptr, 0, nullptr,
                                     0)) {
      return fail(ErrorKind::Io, "cannot add " + e.name + " to the bundle (" +
                                     mz_zip_get_error_string(mz_zip_get_last_error(&writer)) + ")");
    }
  }
  void* buffer = nullptr;
  std::size_t size = 0;
  if (!mz_zip_writer_finalize_heap_archive(&writer, &buffer, &size)) {
    return fail(ErrorKind::Io, "cannot finish the bundle archive");
  }
  auto written = write_file_atomic(out, std::string_view(static_cast<const char*>(buffer), size), Durability::Crash);
  mz_free(buffer);
  return written;
}

std::string safe_name(std::string_view s) {
  std::string out;
  for (const char c : s) out += (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '.') ? c : '_';
  return out.empty() ? std::string("host") : out;
}

}  // namespace

Result<std::filesystem::path> write_support_bundle(const SiteInstall* install, const BundleOptions& options) {
  Collector c(options.max_bytes);
  std::vector<std::string> notes;
  const HostFacts host = host_facts();
  // Stamps written into the bundle follow the session's clock (simulated time
  // in a simulated session); file times stay on real time (Collector::now).
  const std::time_t stamp = SysClock::to_time_t(options.clock ? options.clock->wall_now() : SysClock::now());

  // A fresh flight recorder dump first, so that it holds what led here.
  std::optional<fs::path> fresh_dump;
  if (options.hub) {
    if (auto dump = options.hub->dump_recorder("bundle")) {
      fresh_dump = *dump;
    } else {
      notes.emplace_back("no fresh flight recorder dump: " + dump.error().what);
    }
  }

  fs::path log_dir = options.log_dir;
  if (log_dir.empty() && fresh_dump) log_dir = fresh_dump->parent_path().parent_path();
  if (log_dir.empty()) {
    std::optional<fs::path> root;
    if (install != nullptr) root = install->root;
    log_dir = default_log_dir(root, host_os(), process_env());
  }

  fs::path out = options.out;
  if (out.empty()) {
    if (log_dir.empty()) return fail(ErrorKind::Config, "no folder for the support bundle: give it a file to write");
    out = log_dir / "bundles" / ("nobion-" + safe_name(host_name()) + "-" + utc(stamp, "%Y%m%dT%H%M%SZ") + ".zip");
  }

  fs::path trace_dir = options.trace_dir;
  if (trace_dir.empty() && install != nullptr) trace_dir = install->root / "traces";
  const fs::path site_path = options.site_path.empty() ? default_site_path() : options.site_path;

  // What is never dropped first, so that the cap knows what it has left.
  if (install != nullptr) collect_install(c, *install, log_dir, trace_dir);
  if (is_link(site_path)) {
    ++c.links_skipped;
  } else if (is_plain_file(site_path)) {
    (void)c.add_file(c.names.contains("config/site.toml") ? "config/site.user.toml" : "config/site.toml", site_path,
                     false);
  }
  c.add_text("doctor.txt", doctor_text(install, options), Redact::Text);
  if (options.live) c.add_text("live.json", options.live(), Redact::Text);
  c.add_text("build.json", build_json(install, options, site_path, host, stamp), Redact::Text);

  if (log_dir.empty()) notes.emplace_back("no log folder found");
  auto logs = log_dir.empty() ? std::vector<Candidate>{} : log_candidates(c, log_dir, options);
  auto others = other_candidates(c, log_dir, trace_dir, options, fresh_dump);
  collect_droppable(c, std::move(logs), std::move(others));

  // Under max_bytes: drop the oldest droppable entry still in until the zip,
  // README included, fits. What is left when nothing more may go is written
  // anyway, and README says so.
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < c.entries.size(); ++i) {
    if (c.entries[i].droppable) order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return c.entries[a].mtime < c.entries[b].mtime; });
  std::vector<bool> dropped_flag(c.entries.size(), false);
  const ReadmeFacts facts{install, host, stamp, options.max_bytes};
  std::size_t next = 0;
  Entry readme_entry;
  for (;;) {
    readme_entry = make_entry("README.txt", redact::in_text(readme(facts, c, notes)), stamp, false);
    if (c.total + readme_entry.cost() <= options.max_bytes) break;
    if (next == order.size()) {
      notes.push_back("still over " + std::to_string(options.max_bytes) +
                      " bytes with every log, dump and trace that may go dropped");
      readme_entry = make_entry("README.txt", redact::in_text(readme(facts, c, notes)), stamp, false);
      break;
    }
    const std::size_t i = order[next++];
    dropped_flag[i] = true;
    c.total -= c.entries[i].cost();
    c.dropped.push_back({c.entries[i].mtime, c.entries[i].name});
  }

  std::vector<Entry> kept;
  kept.reserve(c.entries.size() + 1);
  kept.push_back(std::move(readme_entry));
  for (std::size_t i = 0; i < c.entries.size(); ++i) {
    if (!dropped_flag[i]) kept.push_back(std::move(c.entries[i]));
  }
  c.entries.clear();

  std::error_code ec;
  if (!out.parent_path().empty()) fs::create_directories(out.parent_path(), ec);
  if (ec) return fail(ErrorKind::Io, "cannot create " + utf8(out.parent_path()) + ": " + ec.message());
  if (auto written = write_zip(kept, out); !written) return fail(written.error());
  if (!mark_as_user_file(out) && options.hub) {
    options.hub->logger("setup.bundle").warn("cannot mark " + utf8(out) + " as a user file");
  }
  return out;
}

}  // namespace nobion::setup
