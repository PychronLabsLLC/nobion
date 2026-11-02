#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>

#include "pychron/core/error.hpp"
#include "pychron/grains/types.hpp"
#include "pychron/vision/camera_backend.hpp"

namespace pychron::grains {

// What moves an axis. `kind` picks the driver ("zaber", "jss57", ...), which
// reads what it needs from `options`.
struct StageController {
  std::string kind;
  std::string port;  // serial port or address
  std::map<std::string, std::string> options;  // the driver's own (baudrate, steps_per_mm, ...)
};

// One axis, in millimetres. Several Zaber axes on one device differ by `axis`;
// a chain of single-axis devices differs by `device`.
struct StageAxis {
  std::string controller;  // a key of StageConfig::controllers
  int device = 1;
  int axis = 1;
  int sign = 1;  // -1: position runs back from max_mm
  double min_mm = 0;
  double max_mm = 0;
};

// An axis left out is moved by hand.
struct StageConfig {
  std::map<std::string, StageController> controllers;
  std::optional<StageAxis> x;
  std::optional<StageAxis> y;
  std::optional<StageAxis> z;
};

struct GrainsConfig {
  vision::CameraRequest camera;
  GrainOptions pipeline;
  StageConfig stage;
};

// The checks of the [pipeline] table, on options held in memory: a Config error
// naming the field that cannot work.
[[nodiscard]] Result<void> validate_options(const GrainOptions& options);

// Reads grains.toml: a [camera] table (backend, device, width, height, fps,
// [camera.options]), an optional [pipeline] table of GrainOptions fields and an
// optional [stage] table ([stage.controllers.<name>], [stage.axes.x|y|z]).
// A key it does not know, a value of the wrong type, or one that cannot work
// is a Config error naming the key.
[[nodiscard]] Result<GrainsConfig> load_config(const std::filesystem::path& path);

}  // namespace pychron::grains
