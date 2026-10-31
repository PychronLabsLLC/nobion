#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "pychron/grains/config.hpp"

namespace pychron::grains {
namespace {

class Config : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("grains_config_") + ::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path write(const std::string& text) const {
    const auto path = dir_ / "grains.toml";
    std::ofstream out(path, std::ios::binary);
    out << text;
    return path;
  }

  std::filesystem::path dir_;
};

const char* const kCamera = "[camera]\nbackend = \"opencv\"\ndevice = \"0\"\n";

TEST_F(Config, DefaultsWhenPipelineTableAbsent) {
  const auto cfg = load_config(write(kCamera));
  ASSERT_TRUE(cfg.has_value());
  EXPECT_EQ(cfg->camera.backend, "opencv");
  EXPECT_EQ(cfg->camera.device, "0");
  EXPECT_EQ(cfg->camera.width, 0);
  const GrainOptions defaults;
  EXPECT_EQ(cfg->pipeline.min_passes, defaults.min_passes);
  EXPECT_DOUBLE_EQ(cfg->pipeline.min_score, defaults.min_score);
  EXPECT_EQ(cfg->pipeline.ring_px, defaults.ring_px);
}

TEST_F(Config, ReadsEveryPipelineKey) {
  const auto cfg = load_config(write(std::string(kCamera) +
                                     "width = 1280\nheight = 720\nfps = 30\n"
                                     "[camera.options]\nexposure_us = 5000\ngain_db = 1.5\nauto = true\nname = \"x\"\n"
                                     "[pipeline]\n"
                                     "min_contrast = 0.02\nmin_reference_contrast = 0.04\nmin_passes = 4\n"
                                     "min_score = 0.9\ntrack_gap_frames = 20\nmax_shift_px = 6.5\n"
                                     "disagree_passes = 3\nmin_fps = 12\nsegment_window = 50\nring_px = 10\n"
                                     "min_area_px = 500\nmin_pass_frames = 6\nmin_ref_speed_px_s = 2.0\n"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().what;
  EXPECT_EQ(cfg->camera.width, 1280);
  EXPECT_EQ(cfg->camera.height, 720);
  EXPECT_DOUBLE_EQ(cfg->camera.fps, 30.0);
  EXPECT_EQ(cfg->camera.options.at("exposure_us"), "5000");
  EXPECT_EQ(cfg->camera.options.at("gain_db"), "1.5");
  EXPECT_EQ(cfg->camera.options.at("auto"), "true");
  EXPECT_EQ(cfg->camera.options.at("name"), "x");
  const auto& p = cfg->pipeline;
  EXPECT_DOUBLE_EQ(p.min_contrast, 0.02);
  EXPECT_DOUBLE_EQ(p.min_reference_contrast, 0.04);
  EXPECT_EQ(p.min_passes, 4);
  EXPECT_DOUBLE_EQ(p.min_score, 0.9);
  EXPECT_EQ(p.track_gap_frames, 20);
  EXPECT_DOUBLE_EQ(p.max_shift_px, 6.5);
  EXPECT_EQ(p.disagree_passes, 3);
  EXPECT_DOUBLE_EQ(p.min_fps, 12.0);
  EXPECT_EQ(p.segment_window, 50);
  EXPECT_EQ(p.ring_px, 10);
  EXPECT_EQ(p.min_area_px, 500);
  EXPECT_EQ(p.min_pass_frames, 6);
  EXPECT_DOUBLE_EQ(p.min_ref_speed_px_s, 2.0);
}

TEST_F(Config, CameraMissingIsAnError) {
  const auto cfg = load_config(write("[pipeline]\nmin_passes = 3\n"));
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().kind, ErrorKind::Config);
  EXPECT_NE(cfg.error().what.find("camera"), std::string::npos);
}

TEST_F(Config, UnknownKeyIsAnError) {
  const auto in_pipeline = load_config(write(std::string(kCamera) + "[pipeline]\nmin_pases = 3\n"));
  ASSERT_FALSE(in_pipeline.has_value());
  EXPECT_NE(in_pipeline.error().what.find("min_pases"), std::string::npos);

  const auto in_camera = load_config(write(std::string(kCamera) + "framerate = 30\n"));
  ASSERT_FALSE(in_camera.has_value());
  EXPECT_NE(in_camera.error().what.find("framerate"), std::string::npos);

  const auto at_top = load_config(write(std::string(kCamera) + "[pipline]\nmin_passes = 3\n"));
  ASSERT_FALSE(at_top.has_value());
  EXPECT_NE(at_top.error().what.find("pipline"), std::string::npos);
}

TEST_F(Config, OutOfRangeIsAnError) {
  const auto score = load_config(write(std::string(kCamera) + "[pipeline]\nmin_score = 1.5\n"));
  ASSERT_FALSE(score.has_value());
  EXPECT_NE(score.error().what.find("min_score"), std::string::npos);

  const auto passes = load_config(write(std::string(kCamera) + "[pipeline]\nmin_passes = 0\n"));
  ASSERT_FALSE(passes.has_value());
  EXPECT_NE(passes.error().what.find("min_passes"), std::string::npos);

  const auto contrast = load_config(write(std::string(kCamera) + "[pipeline]\nmin_contrast = 1.5\n"));
  ASSERT_FALSE(contrast.has_value());
  EXPECT_NE(contrast.error().what.find("min_contrast"), std::string::npos);
}

TEST_F(Config, WrongTypeIsAnError) {
  const auto cfg = load_config(write(std::string(kCamera) + "[pipeline]\nmin_passes = \"three\"\n"));
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(cfg.error().what.find("min_passes"), std::string::npos);
}

TEST_F(Config, MissingFileIsAnError) {
  const auto cfg = load_config(dir_ / "absent.toml");
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().kind, ErrorKind::Io);
}

}  // namespace
}  // namespace pychron::grains
