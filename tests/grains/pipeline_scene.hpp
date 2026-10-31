#pragma once

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "pychron/grains/pipeline.hpp"
#include "pychron/grains/synth.hpp"
#include "pychron/vision/frame.hpp"

// The scene the pipeline tests share: three grains and a reference, small
// enough to render a sweep in a few seconds in Debug. Under ctest each case is
// its own process, so the cache below helps only a direct run of the binary.
namespace pychron::grains::scene {

using vision::Frame;
using vision::Vec2;

constexpr double kFps = 20.0;
constexpr int kFrames = 200;
constexpr int kClickFrame = 12;  // enough for the segmenter and a fringe history

constexpr double kSanidine = -0.017;
constexpr double kQuartz = 0.010;
constexpr double kNearOil = 0.0005;

constexpr Vec2 kLow{80, 65};
constexpr Vec2 kHigh{240, 65};
constexpr Vec2 kNear{80, 175};
constexpr Vec2 kRef{240, 175};

inline GrainScene four_grains(double reference_delta_n) {
  GrainScene s;
  s.width = 320;
  s.height = 240;
  s.grains.push_back({kLow, 25, 1.0, kSanidine});
  s.grains.push_back({kHigh, 25, 1.0, kQuartz});
  s.grains.push_back({kNear, 25, 1.0, kNearOil});
  s.grains.push_back({kRef, 25, 1.0, reference_delta_n});
  return s;
}

inline std::vector<Frame> render(const GrainScene& s, const ZScript& z, double fps, int frames, const ShiftScript& shift = {}) {
  GrainSynth synth(s, z, fps, shift);
  std::vector<Frame> out;
  out.reserve(static_cast<std::size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    auto f = synth.grab();
    if (!f) {
      ADD_FAILURE() << "grab failed";
      return out;
    }
    out.push_back(std::move(*f));
  }
  return out;
}

// Frames of a script, rendered once per process. By value: a copy costs
// nothing here, and a reference into the cache trips -Wdangling-reference.
inline std::vector<Frame> cached(const std::string& key, const std::function<std::vector<Frame>()>& make) {
  static std::map<std::string, std::vector<Frame>> frames;
  auto it = frames.find(key);
  if (it == frames.end()) it = frames.emplace(key, make()).first;
  return it->second;
}

inline std::vector<Frame> sweep_frames(double reference_delta_n, const std::string& key = "triangle", const ZScript& z = triangle_z(2, 1)) {
  return cached(key + std::to_string(reference_delta_n), [&] { return render(four_grains(reference_delta_n), z, kFps, kFrames); });
}

// Frames [from, to) of `frames`, zero-based; the report of the last one.
inline GrainReport feed(GrainPipeline& p, const std::vector<Frame>& frames, std::size_t from, std::size_t to) {
  GrainReport last;
  for (std::size_t i = from; i < std::min(to, frames.size()); ++i) last = p.process(frames[i]);
  return last;
}

inline Vec2 middle(const GrainView& g) {
  Vec2 sum;
  for (const Vec2& p : g.outline) {
    sum.x += p.x;
    sum.y += p.y;
  }
  const auto n = static_cast<double>(std::max<std::size_t>(g.outline.size(), 1));
  return {sum.x / n, sum.y / n};
}

// The grain whose outline is centered within 20 px of `at`.
inline const GrainView* grain_at(const GrainReport& r, Vec2 at) {
  const GrainView* best = nullptr;
  double closest = 20.0;
  for (const GrainView& g : r.grains) {
    const Vec2 c = middle(g);
    const double d = std::hypot(c.x - at.x, c.y - at.y);
    if (d < closest) {
      closest = d;
      best = &g;
    }
  }
  return best;
}

inline Label label_at(const GrainReport& r, Vec2 at) {
  const GrainView* g = grain_at(r, at);
  if (g == nullptr) {
    ADD_FAILURE() << "no grain at " << at.x << "," << at.y;
    return Label::None;
  }
  return g->label;
}

inline int passes_at(const GrainReport& r, Vec2 at) {
  const GrainView* g = grain_at(r, at);
  if (g == nullptr) {
    ADD_FAILURE() << "no grain at " << at.x << "," << at.y;
    return -1;
  }
  return g->passes_agree + g->passes_against + g->passes_none;
}

inline int total_passes(const GrainReport& r) {
  int n = 0;
  for (const GrainView& g : r.grains) n += g.passes_agree + g.passes_against + g.passes_none;
  return n;
}

// Clicks the reference at kClickFrame, runs the rest; the last report.
inline GrainReport labelled_run(const std::vector<Frame>& frames, Vec2 at, Side side, const GrainOptions& options = {}) {
  GrainPipeline p(options);
  feed(p, frames, 0, kClickFrame);
  const auto clicked = p.set_reference(at, side);
  EXPECT_TRUE(clicked.has_value()) << (clicked ? "" : clicked.error().what);
  return feed(p, frames, kClickFrame, frames.size());
}

}  // namespace pychron::grains::scene
