#include "pychron/grains/synth.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <random>

namespace pychron::grains {
namespace {

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

// Gaussian noise from raw mt19937 words and our own Box-Muller: the standard
// distributions are implementation-defined, and frames must match everywhere.
class Noise {
 public:
  Noise(std::uint32_t seed, double sigma) : rng_(seed), sigma_(sigma) {}
  double next() {
    if (sigma_ <= 0) return 0;
    if (have_spare_) {
      have_spare_ = false;
      return spare_ * sigma_;
    }
    const double u1 = (static_cast<double>(rng_()) + 0.5) / 4294967296.0;
    const double u2 = (static_cast<double>(rng_()) + 0.5) / 4294967296.0;
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double a = 2.0 * std::numbers::pi * u2;
    spare_ = r * std::sin(a);
    have_spare_ = true;
    return r * std::cos(a) * sigma_;
  }

 private:
  std::mt19937 rng_;
  double sigma_;
  double spare_ = 0;
  bool have_spare_ = false;
};

std::uint16_t quantise(double frac, std::uint16_t depth) {
  if (!std::isfinite(frac)) return 0;
  return static_cast<std::uint16_t>(std::round(clamp01(frac) * depth));
}

constexpr double kMinRadiusPx = 1e-3;

// Signed distance (px, positive outside) from the ellipse outline, first
// order: (f - 1) / |grad f| for f the normalised radius.
double signed_distance(double dx, double dy, double a, double b) {
  const double f = std::hypot(dx / a, dy / b);
  if (f < 1e-9) return -std::min(a, b);
  const double gx = dx / (a * a * f), gy = dy / (b * b * f);
  return (f - 1.0) / std::hypot(gx, gy);
}

}  // namespace

ZScript triangle_z(double period_s, double amplitude) {
  return [period_s, amplitude](double t_s) {
    if (!(period_s > 0)) return 0.0;
    const double phase = t_s / period_s - std::floor(t_s / period_s);  // [0, 1)
    const double tri = phase < 0.25   ? 4.0 * phase
                       : phase < 0.75 ? 2.0 - 4.0 * phase
                                      : 4.0 * phase - 4.0;
    return amplitude * tri;
  };
}

vision::Frame render_scene(const GrainScene& s, double z, vision::Vec2 shift) {
  vision::Frame f = vision::Frame::make(s.width, s.height, s.pixel_depth);
  Noise noise(s.seed, s.noise);
  const double sigma = std::max(s.fringe_sigma_px, 1e-3);

  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      double level = s.oil_level;
      for (const SynthGrain& g : s.grains) {
        const double b = std::max(g.radius_px, kMinRadiusPx);
        const double a = std::max(g.elongation * g.radius_px, kMinRadiusPx);
        const double d = signed_distance(x - (g.center.x + shift.x), y - (g.center.y + shift.y), a, b);
        // 1 px linear edge, symmetric about the outline.
        level -= s.edge_level * clamp01(0.5 - d);
        if (g.delta_n == 0) continue;
        const double offset = (g.delta_n < 0 ? 1.0 : -1.0) * s.fringe_px_per_z * z;
        const double u = (d - offset) / sigma;
        if (std::abs(u) > 6.0) continue;
        level += std::abs(g.delta_n) * s.fringe_gain * std::exp(-0.5 * u * u);
      }
      f.at(x, y) = quantise(level + noise.next(), s.pixel_depth);
    }
  }
  return f;
}

GrainSynth::GrainSynth(GrainScene scene, ZScript z, double fps, ShiftScript shift)
    : scene_(std::move(scene)), z_(std::move(z)), fps_(fps), shift_(std::move(shift)) {}

Result<vision::Frame> GrainSynth::grab() {
  ++seq_;
  const double t = static_cast<double>(seq_) / fps_;
  vision::Frame f = render_scene(scene_, z_ ? z_(t) : 0.0, shift_ ? shift_(t) : vision::Vec2{});
  f.seq = seq_;
  f.timestamp = TimePoint{} + Duration(std::llround(static_cast<double>(seq_) * 1e9 / fps_));
  return f;
}

vision::FrameInfo GrainSynth::info() const {
  return {scene_.width, scene_.height, scene_.pixel_depth, fps_};
}

}  // namespace pychron::grains
