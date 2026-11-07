#include "nobion/core/atomic_file.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

std::string slurp(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void spit(const fs::path& p, const std::string& s) {
  std::ofstream out(p, std::ios::binary);
  out << s;
}

struct FailAt {
  explicit FailAt(int step) { nobion::testing::fail_atomic_write_at(step); }
  ~FailAt() { nobion::testing::fail_atomic_write_at(0); }
  FailAt(const FailAt&) = delete;
  FailAt& operator=(const FailAt&) = delete;
};

class AtomicFile : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() /
           (std::string("nobion_atomic_") +
            ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    fs::permissions(dir_, fs::perms::owner_all, fs::perm_options::add, ec);
    fs::remove_all(dir_, ec);
  }
  [[nodiscard]] std::size_t count_temps() const {
    std::size_t n = 0;
    for (const auto& e : fs::directory_iterator(dir_)) {
      if (e.path().filename().string().find(".tmp") != std::string::npos) {
        ++n;
      }
    }
    return n;
  }
  fs::path dir_;
};

TEST_F(AtomicFile, WritesNewContent) {
  const auto p = dir_ / "f.txt";
  spit(p, "old");
  ASSERT_TRUE(nobion::write_file_atomic(p, "new").has_value());
  EXPECT_EQ(slurp(p), "new");
  EXPECT_EQ(count_temps(), 0U);
}

TEST_F(AtomicFile, CreatesAFileThatWasNotThere) {
  const auto p = dir_ / "g.txt";
  ASSERT_TRUE(nobion::write_file_atomic(p, "hello", nobion::Durability::Crash).has_value());
  EXPECT_EQ(slurp(p), "hello");
}

TEST_F(AtomicFile, AFailedWriteLeavesTheOldFile) {
  const auto p = dir_ / "f.txt";
  for (int step = 1; step <= 4; ++step) {
    spit(p, "old");
    const FailAt guard(step);
    const auto r = nobion::write_file_atomic(p, "new");
    ASSERT_FALSE(r.has_value()) << "step " << step;
    EXPECT_EQ(r.error().kind, nobion::ErrorKind::Io) << "step " << step;
    EXPECT_EQ(slurp(p), "old") << "step " << step;
    EXPECT_EQ(count_temps(), 0U) << "step " << step;
  }
}

TEST_F(AtomicFile, AFailureAfterRenameLeavesTheNewFile) {
  const auto p = dir_ / "f.txt";
  spit(p, "old");
  const FailAt guard(5);
  const auto r = nobion::write_file_atomic(p, "new");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, nobion::ErrorKind::Io);
  EXPECT_EQ(slurp(p), "new");
  EXPECT_EQ(count_temps(), 0U);
}

TEST_F(AtomicFile, CrashDurabilitySkipsTheSyncSteps) {
  for (const int step : {3, 5}) {
    const auto p = dir_ / "c.txt";
    spit(p, "old");
    const FailAt guard(step);
    ASSERT_TRUE(nobion::write_file_atomic(p, "new", nobion::Durability::Crash).has_value())
        << "step " << step;
    EXPECT_EQ(slurp(p), "new");
  }
}

TEST_F(AtomicFile, ANonAsciiFileNameWorks) {
  const auto p = dir_ / fs::path(u8"\u00e9tat-\u00e9-\u65e5\u672c.toml");
  spit(p, "old");
  ASSERT_TRUE(nobion::write_file_atomic(p, "new").has_value());
  EXPECT_EQ(slurp(p), "new");
  EXPECT_EQ(count_temps(), 0U);
}

#ifndef _WIN32
TEST_F(AtomicFile, ReplacingKeepsPermissionBits) {
  const auto p = dir_ / "m.txt";
  spit(p, "old");
  fs::permissions(p, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
  ASSERT_TRUE(nobion::write_file_atomic(p, "new").has_value());
  EXPECT_EQ(fs::status(p).permissions(), fs::perms::owner_read | fs::perms::owner_write);
  EXPECT_EQ(slurp(p), "new");
}

TEST_F(AtomicFile, ReadOnlyDirectoryIsAnIoError) {
  if (::geteuid() == 0) {
    GTEST_SKIP() << "root ignores directory permissions";
  }
  const auto p = dir_ / "f.txt";
  spit(p, "old");
  fs::permissions(dir_, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace);
  const auto r = nobion::write_file_atomic(p, "new");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().kind, nobion::ErrorKind::Io);
  EXPECT_EQ(slurp(p), "old");
}
#endif

}  // namespace
