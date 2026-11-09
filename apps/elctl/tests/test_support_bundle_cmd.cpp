// elctl support-bundle (field diagnostics spec section 8): the zip a lab
// sends back, written for an install made by `elctl init` in the test's own
// folder (the fixture's site config), with its path printed.

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "elctl_fixture.hpp"
#include "zip_entries.hpp"

namespace elctl::testing {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class ElctlSupportBundle : public ElctlTest {
 protected:
  fs::path root() const { return path("lab"); }

  void SetUp() override {
    ElctlTest::SetUp();
    const auto o = run_raw({"init", "argus", "--root", root().string(), "--name", "lab", "--yes"});
    ASSERT_EQ(o.code, 0) << o.out << o.err;
  }

  static void put(const fs::path& file, const std::string& text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
  }

  static bool has_prefix(const std::map<std::string, std::string>& entries, const std::string& prefix) {
    for (const auto& [name, bytes] : entries) {
      if (name.starts_with(prefix)) return true;
    }
    return false;
  }
};

TEST_F(ElctlSupportBundle, WritesTheZipAndPrintsItsPath) {
  put(root() / "logs" / "nobion.log", "2026-11-07T10:00:00Z [info] ui: hello from the log\n");
  const fs::path out = path("out") / "bundle.zip";
  const auto o = run_raw({"support-bundle", "--out", out.string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, out.string())) << o.out;
  ASSERT_TRUE(fs::exists(out));

  const auto entries = nobion::testing::zip_entries(out);
  ASSERT_TRUE(entries.has_value());
  for (const char* name : {"README.txt", "build.json", "doctor.txt", "config/install.toml", "logs/nobion.log"}) {
    EXPECT_TRUE(entries->contains(name)) << name;
  }
  // The install was found: doctor ran against it.
  EXPECT_TRUE(contains(entries->at("doctor.txt"), "lab files")) << entries->at("doctor.txt");
  EXPECT_TRUE(contains(entries->at("logs/nobion.log"), "hello from the log"));
  // elctl has no running app to ask.
  EXPECT_FALSE(entries->contains("live.json"));
}

TEST_F(ElctlSupportBundle, SinceAndNoTracesAreHonoured) {
  const fs::path logs = root() / "logs";
  put(logs / "nobion.log", "current\n");
  put(logs / "nobion.1.log", "half an hour old\n");
  put(logs / "nobion.2.log", "three hours old\n");
  const auto now = fs::file_time_type::clock::now();
  fs::last_write_time(logs / "nobion.1.log", now - 30min);
  fs::last_write_time(logs / "nobion.2.log", now - 3h);
  put(root() / "traces" / "ngx.trace", "# a wire trace\n");

  // The defaults: three days of logs, traces in. --install before the command.
  const fs::path all = path("all.zip");
  auto o = run_raw({"--install", "lab", "support-bundle", "--out", all.string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  auto entries = nobion::testing::zip_entries(all);
  ASSERT_TRUE(entries.has_value());
  EXPECT_TRUE(entries->contains("logs/nobion.2.log"));
  EXPECT_TRUE(entries->contains("traces/ngx.trace"));

  // An hour of logs and no traces. --install after it.
  const fs::path some = path("some.zip");
  o = run_raw({"support-bundle", "--install", "lab", "--since", "1h", "--no-traces", "--out", some.string()});
  ASSERT_EQ(o.code, 0) << o.out << o.err;
  entries = nobion::testing::zip_entries(some);
  ASSERT_TRUE(entries.has_value());
  EXPECT_TRUE(entries->contains("logs/nobion.log"));
  EXPECT_TRUE(entries->contains("logs/nobion.1.log"));
  EXPECT_FALSE(entries->contains("logs/nobion.2.log"));
  EXPECT_FALSE(has_prefix(*entries, "traces/"));
}

TEST_F(ElctlSupportBundle, MistakesAreUsageErrors) {
  auto o = run_raw({"support-bundle", "--since", "soon"});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "--since")) << o.err;
  // A bare number would be seconds: refused, so 72 is never read as 72 s.
  o = run_raw({"support-bundle", "--since", "72", "--out", path("bare.zip").string()});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "unit")) << o.err;
  EXPECT_FALSE(fs::exists(path("bare.zip")));
  o = run_raw({"support-bundle", "--out"});
  EXPECT_EQ(o.code, 2);
  o = run_raw({"support-bundle", "--probe"});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "usage: elctl support-bundle")) << o.err;
  o = run_raw({"support-bundle", "--install", "nowhere", "--out", path("x.zip").string()});
  EXPECT_EQ(o.code, 1);
  EXPECT_TRUE(contains(o.err, "nowhere")) << o.err;
  EXPECT_FALSE(fs::exists(path("x.zip")));
}

}  // namespace
}  // namespace elctl::testing
