#include "nobion/setup/support_bundle.hpp"

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
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
#include "nobion/core/host_name.hpp"
#include "nobion/core/log_dir.hpp"
#include "nobion/core/log_hub.hpp"
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

enum class Redact { Text, Toml, None };

struct Entry {
  std::string name;               // zip path
  std::string payload;            // the bytes stored: deflated, or as is
  std::uint64_t raw_size = 0;
  mz_uint32 crc = 0;
  bool deflated = false;
  std::time_t mtime = 0;
  bool droppable = false;         // may go to keep under max_bytes
  [[nodiscard]] std::uint64_t cost() const { return payload.size() + 2 * name.size() + kEntryOverhead; }
};

std::time_t to_time_t(fs::file_time_type t) {
  const auto sys = SysClock::now() + std::chrono::duration_cast<SysClock::duration>(t - fs::file_time_type::clock::now());
  return SysClock::to_time_t(sys);
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

std::optional<std::string> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (in.bad()) return std::nullopt;
  return text;
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

struct Collector {
  std::vector<Entry> entries;
  std::vector<std::string> left_out;  // "path: why"
  std::time_t now = SysClock::to_time_t(SysClock::now());

  void add_text(const std::string& name, std::string_view text, Redact how, bool droppable = false,
                std::optional<std::time_t> mtime = std::nullopt) {
    entries.push_back(make_entry(name, redacted(text, how), mtime.value_or(now), droppable));
  }

  // Reads `path` and adds it under `name`; a file that cannot be read is named in README.
  void add_file(const std::string& name, const fs::path& path, bool droppable) {
    auto text = read_file(path);
    if (!text) {
      left_out.push_back(path.string() + ": cannot be read");
      return;
    }
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    add_text(name, *text, redaction_for(path), droppable, ec ? now : to_time_t(t));
  }
};

std::string json_escape(std::string_view s) {
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
          static constexpr std::string_view kHex = "0123456789abcdef";
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
  const std::string stem = p.stem().string();
  const auto dot = stem.rfind('.');
  if (dot == std::string::npos || dot + 1 == stem.size()) return false;
  return std::all_of(stem.begin() + static_cast<std::ptrdiff_t>(dot) + 1, stem.end(),
                     [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; });
}

std::vector<fs::path> regular_files(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) return out;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code fe;
    if (it->is_regular_file(fe)) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

void collect_logs(Collector& c, const fs::path& log_dir, const BundleOptions& options,
                  const std::optional<fs::path>& fresh_dump) {
  const std::time_t oldest = c.now - std::chrono::duration_cast<std::chrono::seconds>(options.logs_since).count();
  std::size_t newest = 0;
  std::optional<std::time_t> newest_time;
  for (const auto& p : regular_files(log_dir)) {
    if (p.extension() != ".log") continue;
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    const std::time_t mtime = ec ? c.now : to_time_t(t);
    if (is_rotated_log(p) && mtime < oldest) {
      c.left_out.push_back("logs/" + p.filename().string() + ": older than " +
                           std::to_string(options.logs_since.count()) + " h");
      continue;
    }
    c.add_file("logs/" + p.filename().string(), p, true);
    if (!newest_time || mtime >= *newest_time) {
      newest_time = mtime;
      newest = c.entries.size() - 1;
    }
  }
  // The newest log is what was happening: never dropped for size.
  if (newest_time && newest < c.entries.size()) c.entries[newest].droppable = false;

  for (const auto& p : regular_files(log_dir / "crash")) c.add_file("logs/crash/" + p.filename().string(), p, true);
  for (const auto& p : regular_files(log_dir / "flight")) c.add_file("logs/flight/" + p.filename().string(), p, true);
  if (fresh_dump && !is_under(*fresh_dump, log_dir / "flight")) {
    c.add_file("logs/flight/" + fresh_dump->filename().string(), *fresh_dump, true);
  }
}

// A listing of a folder: relative name, size, age; never a body.
std::string listing(const fs::path& dir, std::time_t now) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return "no folder " + dir.generic_string() + "\n";
  std::vector<std::string> lines;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       !ec && it != end; it.increment(ec)) {
    std::error_code fe;
    if (!it->is_regular_file(fe)) continue;
    const auto size = it->file_size(fe);
    const auto t = it->last_write_time(fe);
    const auto age = fe ? 0 : now - to_time_t(t);
    lines.push_back(it->path().lexically_relative(dir).generic_string() + "\t" + std::to_string(size) + " bytes\t" +
                    std::to_string(age) + " s old");
  }
  std::sort(lines.begin(), lines.end());
  std::string out = dir.generic_string() + ": " + std::to_string(lines.size()) + " files\n";
  for (const auto& l : lines) out += l + "\n";
  return out;
}

struct ConfigScan {
  std::vector<fs::path> config;  // relative to root
  std::vector<fs::path> state;   // *.state.toml, relative to root
};

ConfigScan scan_root(Collector& c, const fs::path& root, const std::vector<fs::path>& skip_dirs) {
  ConfigScan scan;
  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  for (const fs::recursive_directory_iterator end; !ec && it != end; it.increment(ec)) {
    const fs::path& p = it->path();
    const std::string name = p.filename().string();
    std::error_code fe;
    if (it->is_directory(fe)) {
      // Hidden folders (.nobion holds the credentials; .git), and folders
      // that are data, logs, traces or field tables (state/ takes those).
      const bool skip = (!name.empty() && name.front() == '.') ||
                        std::any_of(skip_dirs.begin(), skip_dirs.end(),
                                    [&](const fs::path& d) { return is_under(p, d); });
      if (skip) it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(fe)) continue;
    if (name == "credentials.toml") continue;  // never copied, wherever it is
    const fs::path rel = p.lexically_relative(root);
    if (ends_with(name, ".state.toml")) {
      scan.state.push_back(rel);
      continue;
    }
    if (const auto size = it->file_size(fe); !fe && size > kMaxConfigFile) {
      c.left_out.push_back("config/" + rel.generic_string() + ": larger than 16 MiB");
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
  const fs::path data = install.data.empty() ? fs::path() : install.path(install.data);
  const fs::path tables = root / "tables";
  const auto scan = scan_root(c, root, {data, log_dir, trace_dir, tables});

  for (const auto& rel : scan.config) c.add_file("config/" + rel.generic_string(), root / rel, false);

  // install.toml and what changed since the install wrote it.
  std::string edited;
  if (auto text = read_file(record_path(root))) {
    c.add_text("config/install.toml", *text, Redact::Toml);
  }
  if (auto record = read_install_record(root)) {
    std::set<std::string> seen;
    for (const auto& [rel, sha] : record->files) {
      seen.insert(rel);
      if (fs::path(rel).filename() == "credentials.toml") continue;
      auto text = read_file(root / rel);
      if (!text) {
        edited += "missing " + rel + "\n";
      } else if (sha256_hex(*text) != sha) {
        edited += "edited " + rel + "\n";
      }
    }
    for (const auto& rel : scan.config) {
      if (!seen.contains(rel.generic_string())) edited += "unknown " + rel.generic_string() + "\n";
    }
    if (edited.empty()) edited = "no file changed since the install\n";
  } else {
    edited = "no install record: " + record.error().what + "\n";
  }
  c.add_text("config/EDITED.txt", edited, Redact::Text);

  // Runtime state.
  for (const auto& rel : scan.state) c.add_file("state/" + rel.generic_string(), root / rel, false);
  if (std::error_code fe; !data.empty() && fs::is_regular_file(data / "executor_state.json", fe)) {
    c.add_file("state/executor_state.json", data / "executor_state.json", false);
  }
  std::error_code ec;
  if (fs::is_directory(tables, ec)) {
    for (fs::directory_iterator t(tables, ec), end; !ec && t != end; t.increment(ec)) {
      std::error_code fe;
      if (!t->is_directory(fe)) continue;
      const fs::path current = t->path() / "current";
      const std::string table = t->path().filename().string();
      auto pointer = read_file(current);
      if (!pointer) continue;
      c.add_text("state/tables/" + table + "/current", *pointer, Redact::Text);
      std::string version = *pointer;
      while (!version.empty() && std::isspace(static_cast<unsigned char>(version.back())) != 0) version.pop_back();
      const fs::path named = fs::path(version).filename();  // a name in the table's folder, never a path out of it
      if (!named.empty() && fs::is_regular_file(t->path() / named, fe)) {
        c.add_file("state/tables/" + table + "/" + named.string(), t->path() / named, false);
      }
    }
  }
  if (!data.empty()) {
    c.add_text("state/outbox.txt", listing(data / "outbox", c.now), Redact::Text);
    c.add_text("state/spool.txt", listing(data / "spool", c.now), Redact::Text);
  }
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

// `"text"`, escaped for JSON.
std::string json_string(std::string_view text) { return '"' + json_escape(text) + '"'; }

std::string build_json(const SiteInstall* install, const BundleOptions& options, const fs::path& site_path,
                       const HostFacts& host) {
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
  field("os", json_string(host.os));
  field("cpu", "{" + json_string("machine") + ": " + json_string(host.machine) + ", " + json_string("threads") + ": " +
                   std::to_string(std::thread::hardware_concurrency()) + "}");
  if (!options.qt_version.empty()) field("qt_version", json_string(options.qt_version));
  if (install != nullptr) {
    field("install", "{" + json_string("name") + ": " + json_string(install->name) + ", " + json_string("profile") + ": " +
                         json_string(install->profile) + ", " + json_string("root") + ": " +
                         json_string(install->root.generic_string()) + "}");
  }
  j << "  " << json_string("site_toml") << ": " << json_string(site_path.generic_string()) << "\n}\n";
  return j.str();
}

std::string readme(const SiteInstall* install, const HostFacts& host, std::time_t now, const BundleOptions& options,
                   const std::vector<std::string>& notes, const std::vector<std::string>& left_out,
                   const std::vector<std::string>& dropped) {
  std::ostringstream r;
  r << "nobion support bundle\n"
    << "=====================\n\n"
    << "What a lab sends back when something goes wrong: logs, crash files, flight\n"
    << "recorder dumps, the install's config and state, wire traces and the doctor\n"
    << "report. Passwords, tokens and keys are masked as " << redact::kMask << "; the credentials\n"
    << "file (.nobion/credentials.toml) is never included. Nothing was uploaded.\n\n"
    << "build    " << build_id() << "\n"
    << "host     " << host_name() << "\n"
    << "os       " << host.os << (host.machine.empty() ? "" : " " + host.machine) << "\n"
    << "written  " << utc(now, "%Y-%m-%dT%H:%M:%SZ") << "\n"
    << "install  " << (install ? install->name + " (" + install->root.generic_string() + ")" : "none") << "\n\n";
  for (const auto& n : notes) r << n << "\n";
  if (!notes.empty()) r << "\n";
  r << "Left out\n--------\n";
  r << ".nobion/credentials.toml: never included\n";
  for (const auto& l : left_out) r << l << "\n";
  if (!dropped.empty()) {
    r << "\nDropped to keep the bundle under " << options.max_bytes << " bytes (oldest first)\n";
    for (const auto& d : dropped) r << "dropped " << d << "\n";
  }
  r << "\nA hung process\n--------------\n"
    << "The stacks of a process that hangs cannot be written from inside it. Take them\n"
    << "with (pid from the Activity Monitor, ps or Task Manager):\n"
    << "  macOS:   sample <pid> 10\n"
    << R"(  Linux:   gdb -p <pid> -batch -ex "thread apply all bt"   or   eu-stack -p <pid>)" << '\n'
    << "  Windows: procdump -ma <pid>\n";
  return r.str();
}

// Zips `entries` in memory and writes the archive to `out` (temporary file, then rename).
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
  Collector c;
  std::vector<std::string> notes;
  const HostFacts host = host_facts();

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
    out = log_dir / "bundles" /
          ("nobion-" + safe_name(host_name()) + "-" + utc(c.now, "%Y%m%dT%H%M%SZ") + ".zip");
  }

  fs::path trace_dir = options.trace_dir;
  if (trace_dir.empty() && install != nullptr) trace_dir = install->root / "traces";
  const fs::path site_path = options.site_path.empty() ? default_site_path() : options.site_path;

  if (!log_dir.empty()) {
    collect_logs(c, log_dir, options, fresh_dump);
  } else {
    notes.emplace_back("no log folder found");
  }
  if (install != nullptr) collect_install(c, *install, log_dir, trace_dir);
  if (std::error_code ec; fs::is_regular_file(site_path, ec)) c.add_file("config/site.toml", site_path, false);
  if (options.include_traces) {
    for (const auto& p : regular_files(trace_dir)) {
      if (p.extension() == ".trace") c.add_file("traces/" + p.filename().string(), p, true);
    }
  } else {
    c.left_out.emplace_back("traces/: not asked for");
  }
  c.add_text("doctor.txt", doctor_text(install, options), Redact::Text);
  if (options.live) c.add_text("live.json", options.live(), Redact::Text);
  c.add_text("build.json", build_json(install, options, site_path, host), Redact::Text);

  // Under max_bytes: drop the oldest droppable entry until the zip, README
  // included, fits. What is left when nothing more may go is written anyway.
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < c.entries.size(); ++i) {
    if (c.entries[i].droppable) order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t a, std::size_t b) { return c.entries[a].mtime < c.entries[b].mtime; });
  std::vector<bool> dropped_flag(c.entries.size(), false);
  std::vector<std::string> dropped;
  std::uint64_t total = kEndOverhead;
  for (const auto& e : c.entries) total += e.cost();
  std::size_t next = 0;
  Entry readme_entry;
  for (;;) {
    std::vector<std::string> readme_notes = notes;
    readme_entry = make_entry("README.txt",
                              redact::in_text(readme(install, host, c.now, options, readme_notes, c.left_out, dropped)),
                              c.now, false);
    if (total + readme_entry.cost() <= options.max_bytes) break;
    if (next == order.size()) {
      notes.push_back("still over " + std::to_string(options.max_bytes) +
                      " bytes with every log, dump and trace that may go dropped");
      readme_entry = make_entry("README.txt",
                                redact::in_text(readme(install, host, c.now, options, notes, c.left_out, dropped)),
                                c.now, false);
      break;
    }
    const std::size_t i = order[next++];
    dropped_flag[i] = true;
    total -= c.entries[i].cost();
    dropped.push_back(c.entries[i].name);
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
  if (ec) return fail(ErrorKind::Io, "cannot create " + out.parent_path().string() + ": " + ec.message());
  if (auto written = write_zip(kept, out); !written) return fail(written.error());
  (void)mark_as_user_file(out);
  return out;
}

}  // namespace nobion::setup
