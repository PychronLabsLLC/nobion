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

// Zaber x and y on one controller.
const char* const kController = "[stage.controllers.xy]\nkind = \"zaber\"\n";
const char* const kAxisX = "[stage.axes.x]\ncontroller = \"xy\"\nmin_mm = 0\nmax_mm = 50\n";

TEST_F(Config, NoStageTableMeansEveryAxisByHand) {
  const auto cfg = load_config(write(kCamera));
  ASSERT_TRUE(cfg.has_value());
  EXPECT_TRUE(cfg->stage.controllers.empty());
  EXPECT_FALSE(cfg->stage.x.has_value());
  EXPECT_FALSE(cfg->stage.y.has_value());
  EXPECT_FALSE(cfg->stage.z.has_value());
}

TEST_F(Config, ReadsEachAxisOnItsController) {
  const auto cfg = load_config(write(std::string(kCamera) +
                                     "[stage.controllers.xy]\nkind = \"zaber\"\nport = \"/dev/ttyUSB0\"\n"
                                     "[stage.controllers.xy.options]\nbaudrate = 9600\n"
                                     "[stage.controllers.focus]\nkind = \"jss57\"\nport = \"/dev/ttyACM0\"\n"
                                     "[stage.axes.x]\ncontroller = \"xy\"\ndevice = 1\naxis = 1\nsign = -1\n"
                                     "min_mm = 0\nmax_mm = 50\n"
                                     "[stage.axes.y]\ncontroller = \"xy\"\ndevice = 2\nmin_mm = -5\nmax_mm = 45.5\n"
                                     "[stage.axes.z]\ncontroller = \"focus\"\nmin_mm = 0\nmax_mm = 10\n"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().what;
  ASSERT_EQ(cfg->stage.controllers.size(), 2U);
  const auto& xy = cfg->stage.controllers.at("xy");
  EXPECT_EQ(xy.kind, "zaber");
  EXPECT_EQ(xy.port, "/dev/ttyUSB0");
  EXPECT_EQ(xy.options.at("baudrate"), "9600");
  EXPECT_EQ(cfg->stage.controllers.at("focus").kind, "jss57");

  ASSERT_TRUE(cfg->stage.x.has_value());
  EXPECT_EQ(cfg->stage.x->controller, "xy");
  EXPECT_EQ(cfg->stage.x->device, 1);
  EXPECT_EQ(cfg->stage.x->axis, 1);
  EXPECT_EQ(cfg->stage.x->sign, -1);
  EXPECT_DOUBLE_EQ(cfg->stage.x->max_mm, 50.0);

  ASSERT_TRUE(cfg->stage.y.has_value());
  EXPECT_EQ(cfg->stage.y->device, 2);
  EXPECT_EQ(cfg->stage.y->axis, 1);
  EXPECT_EQ(cfg->stage.y->sign, 1);
  EXPECT_DOUBLE_EQ(cfg->stage.y->min_mm, -5.0);

  ASSERT_TRUE(cfg->stage.z.has_value());
  EXPECT_EQ(cfg->stage.z->controller, "focus");
}

TEST_F(Config, AnAxisLeftOutStaysByHand) {
  const auto cfg = load_config(write(std::string(kCamera) + kController + kAxisX));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().what;
  EXPECT_TRUE(cfg->stage.x.has_value());
  EXPECT_FALSE(cfg->stage.z.has_value());
}

TEST_F(Config, AnAxisOnAControllerNotDefinedIsAnError) {
  const auto cfg = load_config(write(std::string(kCamera) + kAxisX));
  ASSERT_FALSE(cfg.has_value());
  EXPECT_EQ(cfg.error().kind, ErrorKind::Config);
  EXPECT_NE(cfg.error().what.find("stage.axes.x"), std::string::npos);
  EXPECT_NE(cfg.error().what.find("xy"), std::string::npos);
}

TEST_F(Config, TwoAxesOnOneMotorIsAnError) {
  const auto cfg = load_config(write(std::string(kCamera) + kController + kAxisX +
                                     "[stage.axes.y]\ncontroller = \"xy\"\nmin_mm = 0\nmax_mm = 50\n"));
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(cfg.error().what.find("same motor"), std::string::npos);
}

TEST_F(Config, BadStageKeysAreNamed) {
  const auto expect_error = [this](const std::string& stage, const std::string& named) {
    const auto cfg = load_config(write(std::string(kCamera) + stage));
    ASSERT_FALSE(cfg.has_value()) << stage;
    EXPECT_EQ(cfg.error().kind, ErrorKind::Config);
    EXPECT_NE(cfg.error().what.find(named), std::string::npos) << cfg.error().what;
  };
  const std::string controller = kController;

  expect_error("[stage]\nchannels = 1\n", "channels");
  expect_error("[stage.controllers.xy]\nport = \"a\"\n", "kind");
  expect_error(controller + "baud = 1\n", "baud");
  expect_error(controller + "[stage.axes.w]\ncontroller = \"xy\"\nmin_mm = 0\nmax_mm = 1\n", "'w'");
  expect_error(controller + "[stage.axes.x]\nmin_mm = 0\nmax_mm = 1\n", "controller");
  expect_error(controller + "[stage.axes.x]\ncontroller = \"xy\"\nmax_mm = 1\n", "min_mm");
  expect_error(controller + "[stage.axes.x]\ncontroller = \"xy\"\nmin_mm = 0\n", "max_mm");
  expect_error(controller + "[stage.axes.x]\ncontroller = \"xy\"\nmin_mm = 1\nmax_mm = 1\n", "max_mm");
  expect_error(controller + kAxisX + "sign = 2\n", "sign");
  expect_error(controller + kAxisX + "device = 0\n", "device");
  expect_error(controller + kAxisX + "speed = 3\n", "speed");
}

TEST_F(Config, TheShippedExampleLoads) {
  const auto cfg = load_config(PYCHRON_GRAINS_EXAMPLE_CONFIG);
  ASSERT_TRUE(cfg.has_value()) << cfg.error().what;
  ASSERT_TRUE(cfg->stage.x.has_value());
  ASSERT_TRUE(cfg->stage.y.has_value());
  ASSERT_TRUE(cfg->stage.z.has_value());
  EXPECT_EQ(cfg->stage.controllers.at(cfg->stage.x->controller).kind, "zaber");
  EXPECT_EQ(cfg->stage.x->controller, cfg->stage.y->controller);
  EXPECT_EQ(cfg->stage.controllers.at(cfg->stage.z->controller).kind, "jss57");
}

}  // namespace
}  // namespace pychron::grains
