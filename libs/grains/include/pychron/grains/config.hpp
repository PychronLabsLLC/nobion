#pragma once

#include <filesystem>

#include "pychron/core/error.hpp"
#include "pychron/grains/types.hpp"
#include "pychron/vision/camera_backend.hpp"

namespace pychron::grains {

struct GrainsConfig {
  vision::CameraRequest camera;
  GrainOptions pipeline;
};

// Reads grains.toml: a [camera] table (backend, device, width, height, fps,
// [camera.options]) and an optional [pipeline] table of GrainOptions fields.
// A key it does not know, a value of the wrong type, or one that cannot work
// is a Config error naming the key.
[[nodiscard]] Result<GrainsConfig> load_config(const std::filesystem::path& path);

}  // namespace pychron::grains
