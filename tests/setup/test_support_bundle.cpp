// The support bundle (field diagnostics spec section 8): one zip a lab sends
// back, with every section, the edited config files, no secret anywhere, and
// under its size cap.

#include <gtest/gtest.h>

#include <miniz.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "nobion/core/build_info.hpp"
#include "nobion/core/clock.hpp"
#include "nobion/core/config/logging_config.hpp"
#include "nobion/core/log_hub.hpp"
#include "nobion/core/logger.hpp"
#include "nobion/setup/install.hpp"
#include "nobion/setup/installer.hpp"
#include "nobion/setup/profile.hpp"
#include "nobion/setup/site.hpp"
#include "nobion/setup/support_bundle.hpp"
#include "temp_path.hpp"

using namespace nobion;
using namespace nobion::setup;
namespace fs = std::filesystem;

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

// Every entry of the zip at `path`, name -> bytes.
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
    out.emplace(stat.m_filename, std::move(data));
  }
  mz_zip_reader_end(&zip);
  return out;
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
  const std::string& readme = entries.at("README.txt");
  EXPECT_NE(readme.find("logs/nobion.5.log"), std::string::npos) << readme;
  EXPECT_NE(readme.find("dropped"), std::string::npos) << readme;
  EXPECT_TRUE(entries.count("config/install.toml")) << "config is never dropped for size";
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
  for (const auto& [name, bytes] : entries) EXPECT_EQ(bytes.find("private"), std::string::npos) << name;
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
