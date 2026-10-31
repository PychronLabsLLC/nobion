#include "pychron/grains/config.hpp"

#include <cmath>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>

namespace pychron::grains {
namespace {

Unexpected<Error> bad(const std::filesystem::path& file, const std::string& what) {
  return fail(ErrorKind::Config, file.string() + ": " + what);
}

bool known(std::string_view key, std::initializer_list<std::string_view> keys) {
  for (const auto k : keys)
    if (k == key) return true;
  return false;
}

// First key of `tbl` not in `keys`, or empty.
std::string unknown_key(const toml::table& tbl, std::initializer_list<std::string_view> keys) {
  for (auto&& kv : tbl)
    if (!known(kv.first.str(), keys)) return std::string(kv.first.str());
  return {};
}

// A number (integer or float) that is finite.
Result<double> number(const toml::node& node, const std::string& name) {
  double v = 0;
  if (const auto* i = node.as_integer()) {
    v = static_cast<double>(i->get());
  } else if (const auto* d = node.as_floating_point()) {
    v = d->get();
  } else {
    return fail(ErrorKind::Config, name + " must be a number");
  }
  if (!std::isfinite(v)) return fail(ErrorKind::Config, name + " must be finite");
  return v;
}

Result<int> whole(const toml::node& node, const std::string& name) {
  const auto* i = node.as_integer();
  if (i == nullptr) return fail(ErrorKind::Config, name + " must be a whole number");
  const std::int64_t v = i->get();
  if (v < -1'000'000'000 || v > 1'000'000'000) return fail(ErrorKind::Config, name + " is out of range");
  return static_cast<int>(v);
}

Result<void> set_double(const toml::table& tbl, const char* key, double& out, double lo, double hi, bool lo_open) {
  const auto* node = tbl.get(key);
  if (node == nullptr) return {};
  const auto v = number(*node, key);
  if (!v) return fail(ErrorKind::Config, v.error().what);
  if ((lo_open ? *v <= lo : *v < lo) || *v > hi) {
    std::ostringstream os;
    os << key << " must be in " << (lo_open ? '(' : '[') << lo << ", " << hi << "]";
    return fail(ErrorKind::Config, os.str());
  }
  out = *v;
  return {};
}

Result<void> set_positive_double(const toml::table& tbl, const char* key, double& out) {
  return set_double(tbl, key, out, 0.0, 1e12, true);
}

Result<void> set_positive_int(const toml::table& tbl, const char* key, int& out) {
  const auto* node = tbl.get(key);
  if (node == nullptr) return {};
  const auto v = whole(*node, key);
  if (!v) return fail(ErrorKind::Config, v.error().what);
  if (*v < 1) return fail(ErrorKind::Config, std::string(key) + " must be at least 1");
  out = *v;
  return {};
}

Result<void> read_pipeline(const toml::table& tbl, GrainOptions& o) {
  if (const auto k = unknown_key(tbl, {"min_contrast", "min_reference_contrast", "min_passes", "min_score",
                                       "track_gap_frames", "max_shift_px", "disagree_passes", "min_fps",
                                       "segment_window", "ring_px", "min_area_px", "min_pass_frames",
                                       "min_ref_speed_px_s"});
      !k.empty())
    return fail(ErrorKind::Config, "unknown key '" + k + "' in [pipeline]");

  if (auto r = set_double(tbl, "min_contrast", o.min_contrast, 0.0, 1.0, false); !r) return r;
  if (auto r = set_double(tbl, "min_reference_contrast", o.min_reference_contrast, 0.0, 1.0, false); !r) return r;
  if (auto r = set_positive_int(tbl, "min_passes", o.min_passes); !r) return r;
  if (auto r = set_double(tbl, "min_score", o.min_score, 0.0, 1.0, true); !r) return r;
  if (auto r = set_positive_int(tbl, "track_gap_frames", o.track_gap_frames); !r) return r;
  if (auto r = set_positive_double(tbl, "max_shift_px", o.max_shift_px); !r) return r;
  if (auto r = set_positive_int(tbl, "disagree_passes", o.disagree_passes); !r) return r;
  if (auto r = set_positive_double(tbl, "min_fps", o.min_fps); !r) return r;
  if (auto r = set_positive_int(tbl, "segment_window", o.segment_window); !r) return r;
  if (auto r = set_positive_int(tbl, "ring_px", o.ring_px); !r) return r;
  if (auto r = set_positive_int(tbl, "min_area_px", o.min_area_px); !r) return r;
  if (auto r = set_positive_int(tbl, "min_pass_frames", o.min_pass_frames); !r) return r;
  return set_positive_double(tbl, "min_ref_speed_px_s", o.min_ref_speed_px_s);
}

Result<void> read_camera(const toml::table& tbl, vision::CameraRequest& c) {
  if (const auto k = unknown_key(tbl, {"backend", "device", "width", "height", "fps", "options"}); !k.empty())
    return fail(ErrorKind::Config, "unknown key '" + k + "' in [camera]");

  const auto backend = tbl["backend"].value<std::string>();
  if (!backend || backend->empty()) return fail(ErrorKind::Config, "[camera] backend missing");
  c.backend = *backend;

  if (const auto* node = tbl.get("device")) {
    const auto device = node->value<std::string>();
    if (!device) return fail(ErrorKind::Config, "device must be a string");
    c.device = *device;
  }
  for (const char* key : {"width", "height"}) {
    const auto* node = tbl.get(key);
    if (node == nullptr) continue;
    const auto v = whole(*node, key);
    if (!v) return fail(ErrorKind::Config, v.error().what);
    if (*v < 0) return fail(ErrorKind::Config, std::string(key) + " must not be negative");
    (std::string_view(key) == "width" ? c.width : c.height) = *v;
  }
  if (const auto* node = tbl.get("fps")) {
    const auto v = number(*node, "fps");
    if (!v) return fail(ErrorKind::Config, v.error().what);
    if (*v < 0) return fail(ErrorKind::Config, "fps must not be negative");
    c.fps = *v;
  }
  if (const auto* node = tbl.get("options")) {
    const auto* options = node->as_table();
    if (options == nullptr) return fail(ErrorKind::Config, "[camera.options] must be a table");
    for (auto&& kv : *options) {
      const std::string key(kv.first.str());
      const auto& val = kv.second;
      if (const auto* s = val.as_string()) {
        c.options[key] = s->get();
      } else if (const auto* i = val.as_integer()) {
        c.options[key] = std::to_string(i->get());
      } else if (const auto* d = val.as_floating_point()) {
        std::ostringstream os;
        os << d->get();
        c.options[key] = os.str();
      } else if (const auto* b = val.as_boolean()) {
        c.options[key] = b->get() ? "true" : "false";
      } else {
        return fail(ErrorKind::Config, "camera option '" + key + "' must be a string, number or boolean");
      }
    }
  }
  return {};
}

}  // namespace

Result<GrainsConfig> load_config(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path.string());
  std::stringstream ss;
  ss << in.rdbuf();

  auto parsed = toml::parse(ss.str());
  if (!parsed) return bad(path, std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();

  if (const auto k = unknown_key(root, {"camera", "pipeline"}); !k.empty())
    return bad(path, "unknown key '" + k + "'");

  GrainsConfig cfg;
  const auto* camera = root["camera"].as_table();
  if (camera == nullptr) return bad(path, "[camera] table missing");
  if (auto r = read_camera(*camera, cfg.camera); !r) return bad(path, r.error().what);

  if (const auto* node = root.get("pipeline")) {
    const auto* pipeline = node->as_table();
    if (pipeline == nullptr) return bad(path, "[pipeline] must be a table");
    if (auto r = read_pipeline(*pipeline, cfg.pipeline); !r) return bad(path, r.error().what);
  }
  return cfg;
}

}  // namespace pychron::grains
