// The support bundle (field diagnostics spec section 8): one zip a lab sends
// back, with every section, the edited config files, no secret anywhere, and
// under its size cap.

#include <gtest/gtest.h>

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "nobion/core/build_info.hpp"
#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/devices/spectrometer/ngx_link.hpp"
#include "nobion/devices/spectrometer/ngx_sim.hpp"
#include "nobion/setup/install.hpp"
#include "nobion/setup/installer.hpp"
#include "nobion/setup/profile.hpp"
#include "nobion/setup/site.hpp"
#include "nobion/setup/support_bundle.hpp"
#include "nobion/transport/sim_transport.hpp"
#include "nobion/transport/trace_recorder.hpp"
#include "temp_path.hpp"

using namespace nobion;
using namespace nobion::setup;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

const fs::path kProfiles(NOBION_PROFILES_DIR);
const fs::path kExamples(NOBION_EXAMPLE_CONFIGS_DIR);

void write(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}

std::string read(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Every entry of the zip at `path`, name -> bytes. Names are unique, and a
// name that is not ASCII is flagged UTF-8 (general-purpose bit 11).
std::map<std::string, std::string> unzip(const fs::path& path) {
  const std::string bytes = read(path);
  std::map<std::string, std::string> out;
  mz_zip_archive zip{};
  EXPECT_TRUE(mz_zip_reader_init_mem(&zip, bytes.data(), bytes.size(), 0)) << path;
  const mz_uint count = mz_zip_reader_get_num_files(&zip);
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
      ADD_FAILURE() << "cannot stat entry " << i;
      continue;
    }
    std::string data(static_cast<std::size_t>(stat.m_uncomp_size), '\0');
    if (!data.empty() && !mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0)) {
      ADD_FAILURE() << "cannot extract " << stat.m_filename;
    }
    const std::string name = stat.m_filename;
    const bool ascii = std::all_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x80; });
    if (!ascii) {
      EXPECT_NE(stat.m_bit_flag & (1U << 11U), 0U) << "not flagged UTF-8: " << name;
    }
    EXPECT_TRUE(out.emplace(name, std::move(data)).second) << "two entries named " << name;
  }
  mz_zip_reader_end(&zip);
  return out;
}

std::string lower_copy(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return s;
}

bool has_prefix(const std::map<std::string, std::string>& entries, std::string_view prefix) {
  return std::any_of(entries.begin(), entries.end(),
                     [&](const auto& e) { return std::string_view(e.first).starts_with(prefix); });
}

// Text that deflate cannot shrink much: random letters and digits.
std::string noise(std::size_t bytes, unsigned seed) {
  static constexpr std::string_view kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::mt19937 gen(seed);
  std::uniform_int_distribution<std::size_t> pick(0, kChars.size() - 1);
  std::string out;
  out.reserve(bytes);
  while (out.size() < bytes) {
    out += (out.size() % 100 == 99) ? '\n' : kChars[pick(gen)];
  }
  return out;
}

// An argus install (as elctl init makes it) in a scratch folder, with its own
// log folder and site config.
struct SupportBundle : ::testing::Test {
  fs::path dir = nobion::testing::unique_temp_path("nobion-bundle-");
  fs::path root = dir / "lab";
  fs::path logs = dir / "logs";
  fs::path out = dir / "out" / "bundle.zip";
  SiteInstall site;

  void SetUp() override {
    auto lib = ProfileLibrary::load(kProfiles, kExamples);
    ASSERT_TRUE(lib) << lib.error().what;
    auto resolved = lib->resolve("argus");
    ASSERT_TRUE(resolved) << resolved.error().what;
    const Answers builtins{{"install_name", Value{std::string("argus")}}, {"root", Value{root.generic_string()}}};
    auto answers = complete_answers(*resolved, {}, builtins);
    ASSERT_TRUE(answers) << answers.error().what;
    auto plan = plan_install(*lib, *resolved, *answers, root);
    ASSERT_TRUE(plan) << plan.error().what;
    auto report = apply_install(*plan);
    ASSERT_TRUE(report) << report.error().what;
    site = site_install(*plan, "argus");
    write(logs / "nobion.log", "2026-11-07T10:00:00.000Z [info] [main] nobion: session start\n");
  }
  void TearDown() override { fs::remove_all(dir); }

  BundleOptions options() const {
    BundleOptions o;
    o.out = out;
    o.log_dir = logs;
    o.site_path = dir / "site.toml";
    return o;
  }
};

TEST_F(SupportBundle, HoldsEverySection) {
  auto written = write_support_bundle(&site, options());
  ASSERT_TRUE(written) << written.error().what;
  EXPECT_EQ(*written, out);
  const auto entries = unzip(out);
  for (const char* name : {"README.txt", "build.json", "config/install.toml", "config/EDITED.txt", "doctor.txt"}) {
    EXPECT_TRUE(entries.count(name)) << name;
  }
  EXPECT_TRUE(has_prefix(entries, "logs/"));
  EXPECT_TRUE(has_prefix(entries, "state/"));
  EXPECT_TRUE(entries.count("logs/nobion.log"));
  EXPECT_TRUE(entries.count("config/defaults.toml"));
  EXPECT_NE(entries.at("build.json").find(build_id()), std::string::npos) << entries.at("build.json");
  EXPECT_NE(entries.at("README.txt").find(build_id()), std::string::npos);
  EXPECT_NE(entries.at("README.txt").find("sample <pid>"), std::string::npos) << "hang-stack commands";
  EXPECT_NE(entries.at("doctor.txt").find('['), std::string::npos) << entries.at("doctor.txt");
  EXPECT_FALSE(entries.count("live.json"));
  // Only the zip is left: no temporary beside it.
  std::vector<fs::path> beside;
  for (const auto& e : fs::directory_iterator(out.parent_path())) beside.push_back(e.path());
  EXPECT_EQ(beside, std::vector<fs::path>{out});
}

TEST_F(SupportBundle, EditedListsChangedFiles) {
  write(root / "defaults.toml", read(root / "defaults.toml") + "\n# edited by the lab\n");
  auto written = write_support_bundle(&site, options());
  ASSERT_TRUE(written) << written.error().what;
  const std::string edited = unzip(out).at("config/EDITED.txt");
  EXPECT_NE(edited.find("edited defaults.toml"), std::string::npos) << edited;
  EXPECT_EQ(edited.find("seed.toml"), std::string::npos) << edited;
}

TEST_F(SupportBundle, NoSecretSurvives) {
  write(root / ".nobion" / "credentials.toml", "[database]\npassword = \"s3cret-A\"\n");
  site.database = "postgres://u:s3cret-B@h/db";
  SiteConfig config;
  config.installs.push_back(site);
  config.default_install = site.name;
  ASSERT_TRUE(save_site(config, dir / "site.toml"));
  ASSERT_NE(read(dir / "site.toml").find("s3cret-B"), std::string::npos);
  write(root / "notifications.toml", "[brevo]\nbrevo_key = \"s3cret-C\"\nsender = \"lab@example.org\"\n");
  write(logs / "nobion.log",
        "2026-11-07T10:00:00.000Z [error] [main] store: cannot open postgres://u:s3cret-D@h/db: refused\n");
  write(root / "sub" / "credentials.toml", "[database]\npassword = \"s3cret-A\"\n");
  write(root / "traces" / "ngx.trace", "# connect amqp://u:s3cret-D@h\n");
  write(logs / "crash" / "nobion-1.txt", "fatal signal 11 at postgres://u:s3cret-D@h/db\n");
  write(root / "extraction_line.state.toml", "[db]\ntoken = \"s3cret-C\"\n");

  auto written = write_support_bundle(&site, options());
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  ASSERT_TRUE(entries.count("config/site.toml"));
  ASSERT_TRUE(entries.count("config/notifications.toml"));
  for (const auto& [name, bytes] : entries) {
    EXPECT_EQ(name.find("credentials"), std::string::npos) << name;
    for (const char* secret : {"s3cret-A", "s3cret-B", "s3cret-C", "s3cret-D"}) {
      EXPECT_EQ(bytes.find(secret), std::string::npos) << secret << " in " << name;
    }
  }
  // The rest of what they said is kept.
  EXPECT_NE(entries.at("config/site.toml").find("postgres://u:***@h/db"), std::string::npos);
  EXPECT_NE(entries.at("logs/nobion.log").find("cannot open postgres://u:***@h/db"), std::string::npos);
  EXPECT_NE(entries.at("config/notifications.toml").find("lab@example.org"), std::string::npos);
  // And the raw zip bytes hold none of them either (names, comments).
  const std::string raw = read(out);
  for (const char* secret : {"s3cret-A", "s3cret-B", "s3cret-C", "s3cret-D"}) {
    EXPECT_EQ(raw.find(secret), std::string::npos) << secret;
  }
}

TEST_F(SupportBundle, KeepsUnderMaxBytes) {
  // Five rotated logs of 1 MiB each, nobion.5.log the oldest, and the current one newest.
  const auto now = fs::file_time_type::clock::now();
  for (int i = 1; i <= 5; ++i) {
    const fs::path p = logs / ("nobion." + std::to_string(i) + ".log");
    write(p, noise(1u << 20, static_cast<unsigned>(i)));
    fs::last_write_time(p, now - std::chrono::hours(i));
  }
  write(logs / "nobion.log", noise(64u << 10, 99));
  fs::last_write_time(logs / "nobion.log", now);

  BundleOptions o = options();
  o.max_bytes = 1u << 20;
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  EXPECT_LE(fs::file_size(out), o.max_bytes);
  const auto entries = unzip(out);
  EXPECT_TRUE(entries.count("logs/nobion.log"));
  EXPECT_FALSE(entries.count("logs/nobion.5.log"));
  // Newest first until the cap: the current log and nobion.1 kept, the rest dropped.
  std::vector<std::string> kept_logs;
  for (const auto& [name, bytes] : entries) {
    if (name.starts_with("logs/")) kept_logs.push_back(name);
  }
  EXPECT_EQ(kept_logs, (std::vector<std::string>{"logs/nobion.1.log", "logs/nobion.log"}));
  const std::string readme = entries.at("README.txt");
  for (int i = 2; i <= 5; ++i) {
    const std::string name = "logs/nobion." + std::to_string(i) + ".log";
    EXPECT_NE(readme.find("dropped " + name), std::string::npos) << name << "\n" << readme;
  }
  EXPECT_EQ(readme.find("dropped logs/nobion.1.log"), std::string::npos) << readme;
  EXPECT_EQ(readme.find("still over"), std::string::npos) << readme;
  EXPECT_TRUE(entries.count("config/install.toml")) << "config is never dropped for size";
}

TEST_F(SupportBundle, TheNewestLogByTimeIsNeverDropped) {
  // The newest log is a rotated one here; the current file is older and goes first.
  const auto now = fs::file_time_type::clock::now();
  write(logs / "nobion.1.log", noise(900u << 10, 1));
  fs::last_write_time(logs / "nobion.1.log", now);
  write(logs / "nobion.log", noise(600u << 10, 2));
  fs::last_write_time(logs / "nobion.log", now - std::chrono::hours(1));
  BundleOptions o = options();
  o.max_bytes = 1u << 20;
  ASSERT_TRUE(write_support_bundle(&site, o));
  const auto entries = unzip(out);
  EXPECT_TRUE(entries.count("logs/nobion.1.log"));
  EXPECT_FALSE(entries.count("logs/nobion.log"));
  EXPECT_NE(entries.at("README.txt").find("dropped logs/nobion.log"), std::string::npos);
}

TEST_F(SupportBundle, StillOverTheCapIsWrittenAndSaysSo) {
  // The newest log is never dropped: its 64 KiB tail and the config are more than the cap.
  write(logs / "nobion.log", noise(128u << 10, 7));
  BundleOptions o = options();
  o.max_bytes = 64u << 10;
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  EXPECT_NE(entries.at("README.txt").find("still over"), std::string::npos) << entries.at("README.txt");
  EXPECT_TRUE(entries.count("logs/nobion.log"));
  EXPECT_TRUE(entries.count("config/install.toml"));
  EXPECT_GT(fs::file_size(out), o.max_bytes);
}

TEST_F(SupportBundle, AFileLargerThanTheCapGivesOnlyItsTail) {
  std::string big;
  for (int i = 0; big.size() < (3u << 20); ++i) big += "line " + std::to_string(i) + " " + noise(90, static_cast<unsigned>(i)) + "\n";
  write(logs / "nobion.log", big);
  BundleOptions o = options();
  o.max_bytes = 1u << 20;
  ASSERT_TRUE(write_support_bundle(&site, o));
  EXPECT_LE(fs::file_size(out), o.max_bytes);
  const auto entries = unzip(out);
  const std::string tail = entries.at("logs/nobion.log");
  ASSERT_LE(tail.size(), o.max_bytes);
  ASSERT_GT(tail.size(), 0U);
  ASSERT_TRUE(tail.starts_with("line ")) << "starts at a line start";
  EXPECT_TRUE(big.ends_with(tail));
  EXPECT_EQ(big[big.size() - tail.size() - 1], '\n');
  EXPECT_NE(entries.at("README.txt").find("cut logs/nobion.log"), std::string::npos) << entries.at("README.txt");
}

TEST_F(SupportBundle, LinksAreNeverFollowed) {
  write(dir / "outside" / "secret.txt", "plain-secret-L\n");
  write(dir / "outside" / "dir" / "inner.toml", "x = \"plain-secret-M\"\n");
  std::error_code ec;
  fs::create_symlink(dir / "outside" / "secret.txt", root / "notes.txt", ec);
  ASSERT_FALSE(ec) << ec.message();
  fs::create_directory_symlink(dir / "outside" / "dir", root / "linked", ec);
  ASSERT_FALSE(ec) << ec.message();
  fs::create_symlink(dir / "outside" / "secret.txt", logs / "linked.log", ec);
  ASSERT_FALSE(ec) << ec.message();
  fs::create_directory_symlink(dir / "outside" / "dir", logs / "crash", ec);
  ASSERT_FALSE(ec) << ec.message();
  BundleOptions o = options();
  fs::create_symlink(dir / "outside" / "secret.txt", dir / "site.toml", ec);
  ASSERT_FALSE(ec) << ec.message();
  ASSERT_TRUE(write_support_bundle(&site, o));
  for (const auto& [name, bytes] : unzip(out)) {
    EXPECT_EQ(bytes.find("plain-secret-"), std::string::npos) << name;
  }
}

TEST_F(SupportBundle, OnlyAllowedConfigFilesAreCopied) {
  for (const char* name : {".env", ".netrc", "id_rsa", "server.pem", "tls.key", "Credentials.toml",
                           "credentials.local.toml", "my_secrets.toml", "sub/credentials.toml", "scripts/x.py",
                           ".hidden/plain.toml"}) {
    write(root / name, std::string("leak = \"leak-") + name + "\"\n");
  }
  write(root / "notes.md", "kept\n");
  ASSERT_TRUE(write_support_bundle(&site, options()));
  const auto entries = unzip(out);
  EXPECT_TRUE(entries.count("config/notes.md"));
  for (const auto& [name, bytes] : entries) {
    EXPECT_EQ(bytes.find("leak-"), std::string::npos) << name;
    for (const char* bad : {".env", ".netrc", "id_rsa", ".pem", "tls.key", "redential", "ecret", ".py"}) {
      EXPECT_EQ(name.find(bad), std::string::npos) << name;
    }
  }
  const std::string readme = entries.at("README.txt");
  for (const char* bad : {".env", ".netrc", "id_rsa", "server.pem", "tls.key", "Credentials.toml", "credentials.local", "sub/credentials", "my_secrets", "x.py", "plain.toml"}) {
    EXPECT_EQ(readme.find(bad), std::string::npos) << bad << "\n" << readme;
  }
  EXPECT_NE(readme.find("config files left out"), std::string::npos) << readme;
}

TEST_F(SupportBundle, ExtensionsAreReadWithoutCase) {
  write(root / "Device.TOML", "password = \"hunter2\"\nport = 4001\n");
  write(root / "Gauge.STATE.TOML", "token = \"hunter3\"\n");
  ASSERT_TRUE(write_support_bundle(&site, options()));
  const auto entries = unzip(out);
  ASSERT_TRUE(entries.count("config/Device.TOML"));
  EXPECT_NE(entries.at("config/Device.TOML").find("port = 4001"), std::string::npos);
  EXPECT_TRUE(entries.count("state/Gauge.STATE.TOML"));
  for (const auto& [name, bytes] : entries) {
    EXPECT_EQ(bytes.find("hunter"), std::string::npos) << name;
  }
}

TEST_F(SupportBundle, ASecretFolderIsNeverCopied) {
  write(root / "secrets" / "api.txt", "token: abc123\n");
  write(root / "Secrets" / "db.toml", "pin = \"1234\"\n");
  write(root / "lab" / "Credentials" / "x.json", R"({"pin":"5678"})");
  ASSERT_TRUE(write_support_bundle(&site, options()));
  for (const auto& [name, bytes] : unzip(out)) {
    EXPECT_EQ(lower_copy(name).find("secret"), std::string::npos) << name;
    EXPECT_EQ(lower_copy(name).find("credential"), std::string::npos) << name;
    EXPECT_EQ(bytes.find("abc123"), std::string::npos) << name;
    EXPECT_EQ(bytes.find("1234\""), std::string::npos) << name;
    EXPECT_EQ(bytes.find("5678"), std::string::npos) << name;
  }
}

TEST_F(SupportBundle, ANameThatIsASecretKeyIsNeverCopied) {
  for (const char* name : {"password.txt", "api_token.json", "passwd.toml", "Token.yaml"}) {
    write(root / name, std::string("leak-") + name + "\n");
  }
  write(root / "keyboard.toml", "layout = \"us\"\n");
  write(root / "monkey.json", R"({"name":"monkey"})");
  ASSERT_TRUE(write_support_bundle(&site, options()));
  const auto entries = unzip(out);
  EXPECT_TRUE(entries.count("config/keyboard.toml"));
  EXPECT_TRUE(entries.count("config/monkey.json"));
  for (const auto& [name, bytes] : entries) {
    EXPECT_EQ(bytes.find("leak-"), std::string::npos) << name;
  }
  for (const char* name : {"password.txt", "api_token.json", "passwd.toml", "Token.yaml"}) {
    EXPECT_FALSE(entries.count(std::string("config/") + name)) << name;
  }
}

TEST_F(SupportBundle, StampsFollowTheGivenClock) {
  // 2030-01-02T03:04:05Z
  ManualClock clock(TimePoint{}, WallTime{std::chrono::seconds(1893553445)});
  BundleOptions o = options();
  o.out.clear();
  o.clock = &clock;
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  EXPECT_NE(written->filename().string().find("-20300102T030405Z.zip"), std::string::npos) << *written;
  const auto entries = unzip(*written);
  EXPECT_NE(entries.at("README.txt").find("2030-01-02T03:04:05Z"), std::string::npos);
  EXPECT_NE(entries.at("build.json").find("2030-01-02T03:04:05Z"), std::string::npos);
  EXPECT_TRUE(entries.count("logs/nobion.log")) << "the log cutoff stays on real time";
}

TEST_F(SupportBundle, NamesAreUtf8) {
  const fs::path name = fs::path(u8"nobion-M\u00fcller.log");
  write(logs / name, "hello\n");
  ASSERT_TRUE(write_support_bundle(&site, options()));
  const auto entries = unzip(out);
  EXPECT_TRUE(entries.count("logs/nobion-M\xc3\xbcller.log"));
}

TEST_F(SupportBundle, OldRotatedLogsAreLeftOut) {
  const fs::path old = logs / "nobion.1.log";
  write(old, "old\n");
  fs::last_write_time(old, fs::file_time_type::clock::now() - std::chrono::hours(100));
  auto written = write_support_bundle(&site, options());
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  EXPECT_FALSE(entries.count("logs/nobion.1.log"));
  EXPECT_NE(entries.at("README.txt").find("nobion.1.log"), std::string::npos);
}

TEST_F(SupportBundle, LiveStateIsIncludedWhenGiven) {
  BundleOptions o = options();
  o.live = [] { return std::string(R"({"executor":"idle","store":"postgres://u:pw-live@h/db"})"); };
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  ASSERT_TRUE(entries.count("live.json"));
  EXPECT_NE(entries.at("live.json").find(R"("executor":"idle")"), std::string::npos);
  EXPECT_EQ(entries.at("live.json").find("pw-live"), std::string::npos);
}

TEST_F(SupportBundle, StateAndTracesAreCollected) {
  write(root / "extraction_line.state.toml", "[valves]\nA = \"open\"\n");
  write(root / "data" / "executor_state.json", R"({"state":"idle"})");
  write(root / "data" / "outbox" / "n-0001.json", R"({"body":"private"})");
  write(root / "traces" / "ngx.trace", "# trace of ngx\n> GetData\n");
  auto written = write_support_bundle(&site, options());
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  EXPECT_TRUE(entries.count("state/extraction_line.state.toml"));
  EXPECT_FALSE(entries.count("config/extraction_line.state.toml"));
  EXPECT_TRUE(entries.count("state/executor_state.json"));
  ASSERT_TRUE(entries.count("state/outbox.txt"));
  EXPECT_NE(entries.at("state/outbox.txt").find("n-0001.json"), std::string::npos);
  for (const auto& [name, bytes] : entries) {
    EXPECT_EQ(bytes.find("private"), std::string::npos) << name;
  }
  EXPECT_TRUE(entries.count("traces/ngx.trace"));

  BundleOptions o = options();
  o.include_traces = false;
  ASSERT_TRUE(write_support_bundle(&site, o));
  EXPECT_FALSE(unzip(out).count("traces/ngx.trace"));
}

TEST_F(SupportBundle, WithoutAnInstallSaysSo) {
  auto written = write_support_bundle(nullptr, options());
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  EXPECT_EQ(entries.at("doctor.txt"), "no install\n");
  EXPECT_TRUE(entries.count("logs/nobion.log"));
  EXPECT_FALSE(entries.count("config/install.toml"));
}

TEST_F(SupportBundle, TakesAFreshDumpFromTheHub) {
  config::LoggingConfig cfg;
  cfg.dir = logs;
  cfg.recorder.size_mb = 4;
  SteadyClock clock;
  auto hub = LogHub::create(cfg, clock);
  ASSERT_TRUE(hub) << hub.error().what;
  (*hub)->logger("test").info("before the bundle");
  BundleOptions o = options();
  o.hub = *hub;
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  const auto dump = std::find_if(entries.begin(), entries.end(), [](const auto& e) {
    return e.first.rfind("logs/flight/", 0) == 0 && e.first.find("-bundle.log") != std::string::npos;
  });
  ASSERT_NE(dump, entries.end());
  EXPECT_NE(dump->second.find("before the bundle"), std::string::npos);
}

// An NGX session that logged in, recorded as a lab records it (the wire
// logger in the flight recorder, a trace file under the install): no entry
// of the bundle holds the password, in text or in hex.
TEST_F(SupportBundle, NoDevicePasswordFromTheWire) {
  config::LoggingConfig cfg;
  cfg.dir = logs;
  cfg.recorder.size_mb = 4;
  SteadyClock clock;
  auto hub = LogHub::create(cfg, clock);
  ASSERT_TRUE(hub) << hub.error().what;
  auto model = std::make_shared<spectrometer::NgxSimModel>();
  model->clock = &clock;
  model->mass = 39.96;
  TransportOptions to;
  to.name = "ngx";
  to.clock = &clock;
  auto sim = SimTransport::hooked(spectrometer::ngx_sim_hook(model), to, spectrometer::ngx_sim_events(model));
  ASSERT_TRUE(sim->open());
  fs::create_directories(root / "traces");
  auto recorder =
      TraceRecorder::to_file(std::move(sim), (root / "traces" / "ngx.trace").string(), clock, (*hub)->logger("ngx.wire"));
  ASSERT_TRUE(recorder);
  {
    spectrometer::NgxLinkOptions lo;
    lo.user = "pychron";
    lo.password = "s3cret-ngx";
    lo.command_timeout = 5s;  // an upper limit only: the sim answers at once
    spectrometer::NgxLink link(**recorder, lo, clock);
    ASSERT_TRUE(link.connect());
    ASSERT_TRUE(link.ask("GETMASS"));
  }
  recorder->reset();

  BundleOptions o = options();
  o.hub = *hub;
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  const auto entries = unzip(out);
  ASSERT_TRUE(entries.count("traces/ngx.trace"));
  const std::string hex = to_hex(to_bytes("s3cret-ngx"));
  const std::string hex_upper = "73 33 63 72 65 74 2D 6E 67 78";
  bool wire_seen = false;
  for (const auto& [name, bytes] : entries) {
    EXPECT_EQ(bytes.find("s3cret-ngx"), std::string::npos) << name;
    EXPECT_EQ(bytes.find(hex), std::string::npos) << name;
    EXPECT_EQ(bytes.find(hex_upper), std::string::npos) << name;
    wire_seen = wire_seen || bytes.find("ngx.wire: tx") != std::string::npos;
  }
  EXPECT_TRUE(wire_seen);  // the session is in the bundle's dump
}

TEST_F(SupportBundle, TheDefaultNameIsUnderTheLogFolder) {
  BundleOptions o = options();
  o.out.clear();
  auto written = write_support_bundle(&site, o);
  ASSERT_TRUE(written) << written.error().what;
  EXPECT_EQ(written->parent_path(), logs / "bundles");
  EXPECT_EQ(written->filename().string().rfind("nobion-", 0), 0u) << *written;
  EXPECT_EQ(written->extension(), ".zip");
  EXPECT_TRUE(fs::exists(*written));
}

}  // namespace
