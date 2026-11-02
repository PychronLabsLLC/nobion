#include "pychron/grains/config.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <initializer_list>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <toml++/toml.hpp>
#include <utility>

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

// The free-form [<owner>.options] table, values kept as text for the driver.
Result<void> read_options(const toml::table& tbl, const std::string& owner, std::map<std::string, std::string>& out) {
  const auto* node = tbl.get("options");
  if (node == nullptr) return {};
  const auto* options = node->as_table();
  if (options == nullptr) return fail(ErrorKind::Config, "[" + owner + ".options] must be a table");
  for (auto&& kv : *options) {
    const std::string key(kv.first.str());
    const auto& val = kv.second;
    if (const auto* s = val.as_string()) {
      out[key] = s->get();
    } else if (const auto* i = val.as_integer()) {
      out[key] = std::to_string(i->get());
    } else if (const auto* d = val.as_floating_point()) {
      std::ostringstream os;
      os << d->get();
      out[key] = os.str();
    } else if (const auto* b = val.as_boolean()) {
      out[key] = b->get() ? "true" : "false";
    } else {
      std::string what = owner + " option '";
      what += key;
      what += "' must be a string, number or boolean";
      return fail(ErrorKind::Config, what);
    }
  }
  return {};
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
  return read_options(tbl, "camera", c.options);
}

Result<void> read_controller(const toml::table& tbl, const std::string& where, StageController& c) {
  if (const auto k = unknown_key(tbl, {"kind", "port", "options"}); !k.empty())
    return fail(ErrorKind::Config, "unknown key '" + k + "' in [" + where + "]");

  const auto kind = tbl["kind"].value<std::string>();
  if (!kind || kind->empty()) return fail(ErrorKind::Config, "[" + where + "] kind missing");
  c.kind = *kind;

  if (const auto* node = tbl.get("port")) {
    const auto port = node->value<std::string>();
    if (!port) return fail(ErrorKind::Config, "[" + where + "] port must be a string");
    c.port = *port;
  }
  return read_options(tbl, where, c.options);
}

Result<void> read_axis(const toml::table& tbl, const std::string& where, StageAxis& a) {
  if (const auto k = unknown_key(tbl, {"controller", "device", "axis", "sign", "min_mm", "max_mm"}); !k.empty())
    return fail(ErrorKind::Config, "unknown key '" + k + "' in [" + where + "]");

  const auto controller = tbl["controller"].value<std::string>();
  if (!controller || controller->empty()) return fail(ErrorKind::Config, "[" + where + "] controller missing");
  a.controller = *controller;

  if (auto r = set_positive_int(tbl, "device", a.device); !r) return r;
  if (auto r = set_positive_int(tbl, "axis", a.axis); !r) return r;
  if (const auto* node = tbl.get("sign")) {
    const auto v = whole(*node, "sign");
    if (!v) return fail(ErrorKind::Config, v.error().what);
    if (*v != 1 && *v != -1) return fail(ErrorKind::Config, "[" + where + "] sign must be 1 or -1");
    a.sign = *v;
  }

  const auto* lo = tbl.get("min_mm");
  if (lo == nullptr) return fail(ErrorKind::Config, "[" + where + "] min_mm missing");
  const auto* hi = tbl.get("max_mm");
  if (hi == nullptr) return fail(ErrorKind::Config, "[" + where + "] max_mm missing");
  const auto min_mm = number(*lo, "min_mm");
  if (!min_mm) return fail(ErrorKind::Config, min_mm.error().what);
  const auto max_mm = number(*hi, "max_mm");
  if (!max_mm) return fail(ErrorKind::Config, max_mm.error().what);
  if (*min_mm >= *max_mm) return fail(ErrorKind::Config, "[" + where + "] min_mm must be below max_mm");
  a.min_mm = *min_mm;
  a.max_mm = *max_mm;
  return {};
}

Result<void> read_stage(const toml::table& tbl, StageConfig& s) {
  if (const auto k = unknown_key(tbl, {"controllers", "axes"}); !k.empty())
    return fail(ErrorKind::Config, "unknown key '" + k + "' in [stage]");

  if (const auto* node = tbl.get("controllers")) {
    const auto* controllers = node->as_table();
    if (controllers == nullptr) return fail(ErrorKind::Config, "[stage.controllers] must be a table");
    for (auto&& kv : *controllers) {
      const std::string name(kv.first.str());
      const std::string where = "stage.controllers." + name;
      const auto* controller = kv.second.as_table();
      if (controller == nullptr) return fail(ErrorKind::Config, "[" + where + "] must be a table");
      if (auto r = read_controller(*controller, where, s.controllers[name]); !r) return r;
    }
  }

  const auto* node = tbl.get("axes");
  if (node == nullptr) return {};
  const auto* axes = node->as_table();
  if (axes == nullptr) return fail(ErrorKind::Config, "[stage.axes] must be a table");
  if (const auto k = unknown_key(*axes, {"x", "y", "z"}); !k.empty())
    return fail(ErrorKind::Config, "unknown axis '" + k + "' in [stage.axes]");

  struct Slot {
    std::string name;
    std::optional<StageAxis>* axis;
  };
  const std::array<Slot, 3> axis_slots{{{"x", &s.x}, {"y", &s.y}, {"z", &s.z}}};
  for (std::size_t i = 0; i < axis_slots.size(); ++i) {
    const auto* axis_node = axes->get(axis_slots[i].name);
    if (axis_node == nullptr) continue;
    const std::string where = "stage.axes." + axis_slots[i].name;
    const auto* axis = axis_node->as_table();
    if (axis == nullptr) return fail(ErrorKind::Config, "[" + where + "] must be a table");
    StageAxis a;
    if (auto r = read_axis(*axis, where, a); !r) return r;
    if (!s.controllers.contains(a.controller))
      return fail(ErrorKind::Config, "[" + where + "] names controller '" + a.controller + "', not defined");
    for (std::size_t j = 0; j < i; ++j) {
      const auto& other = *axis_slots[j].axis;
      if (other && other->controller == a.controller && other->device == a.device && other->axis == a.axis)
        return fail(ErrorKind::Config, "[" + where + "] and [stage.axes." + axis_slots[j].name + "] are the same motor");
    }
    *axis_slots[i].axis = std::move(a);
  }
  return {};
}

}  // namespace

Result<void> validate_options(const GrainOptions& o) {
  const toml::table tbl{{"min_contrast", o.min_contrast},
                        {"min_reference_contrast", o.min_reference_contrast},
                        {"min_passes", o.min_passes},
                        {"min_score", o.min_score},
                        {"track_gap_frames", o.track_gap_frames},
                        {"max_shift_px", o.max_shift_px},
                        {"disagree_passes", o.disagree_passes},
                        {"min_fps", o.min_fps},
                        {"segment_window", o.segment_window},
                        {"ring_px", o.ring_px},
                        {"min_area_px", o.min_area_px},
                        {"min_pass_frames", o.min_pass_frames},
                        {"min_ref_speed_px_s", o.min_ref_speed_px_s}};
  GrainOptions scratch;
  return read_pipeline(tbl, scratch);
}

Result<GrainsConfig> load_config(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail(ErrorKind::Io, "cannot open " + path.string());
  std::stringstream ss;
  ss << in.rdbuf();

  auto parsed = toml::parse(ss.str());
  if (!parsed) return bad(path, std::string(parsed.error().description()));
  const toml::table root = std::move(parsed).table();

  if (const auto k = unknown_key(root, {"camera", "pipeline", "stage"}); !k.empty())
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

  if (const auto* node = root.get("stage")) {
    const auto* stage = node->as_table();
    if (stage == nullptr) return bad(path, "[stage] must be a table");
    if (auto r = read_stage(*stage, cfg.stage); !r) return bad(path, r.error().what);
  }
  return cfg;
}

}  // namespace pychron::grains
