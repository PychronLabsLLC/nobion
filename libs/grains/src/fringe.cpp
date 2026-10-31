#include "pychron/grains/fringe.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <vector>

namespace pychron::grains {
namespace {

constexpr int kAngles = 64;
constexpr double kMinContrastPx = 1e-9;
// Share of the peak below which the residual no longer counts toward position.
constexpr double kPeakShare = 0.25;

// Outline radius at the center of each of kAngles bins about the centroid:
// the mean radius of the boundary points in it, empty bins filled by linear
// interpolation round the circle. False when no bin has a point.
bool outline_radii(const vision::Component& c, std::vector<double>& radius) {
  std::vector<double> sum(kAngles, 0.0);
  std::vector<int> count(kAngles, 0);
  for (const vision::Vec2& p : c.boundary) {
    const double dx = p.x - c.centroid.x, dy = p.y - c.centroid.y;
    const double r = std::hypot(dx, dy);
    if (r < 1e-9) continue;
    const double a = std::atan2(dy, dx) + std::numbers::pi;  // [0, 2 pi]
    const int bin = std::min(kAngles - 1, static_cast<int>(a / (2 * std::numbers::pi) * kAngles));
    sum[static_cast<std::size_t>(bin)] += r;
    ++count[static_cast<std::size_t>(bin)];
  }
  radius.assign(kAngles, 0.0);
  std::vector<int> filled;
  for (int i = 0; i < kAngles; ++i) {
    if (count[static_cast<std::size_t>(i)] > 0) {
      radius[static_cast<std::size_t>(i)] = sum[static_cast<std::size_t>(i)] / count[static_cast<std::size_t>(i)];
      filled.push_back(i);
    }
  }
  if (filled.empty()) return false;
  for (int i = 0; i < kAngles; ++i) {
    if (count[static_cast<std::size_t>(i)] > 0) continue;
    int lo = i, hi = i, dlo = 0, dhi = 0;
    while (count[static_cast<std::size_t>(lo)] == 0) {
      lo = (lo + kAngles - 1) % kAngles;
      ++dlo;
    }
    while (count[static_cast<std::size_t>(hi)] == 0) {
      hi = (hi + 1) % kAngles;
      ++dhi;
    }
    const double t = static_cast<double>(dlo) / static_cast<double>(dlo + dhi);
    radius[static_cast<std::size_t>(i)] = (1 - t) * radius[static_cast<std::size_t>(lo)] + t * radius[static_cast<std::size_t>(hi)];
  }
  return true;
}

// Bilinear sample; the caller keeps (x, y) inside the frame.
double sample(const vision::FrameView& f, double x, double y) {
  const int x0 = std::min(static_cast<int>(std::floor(x)), f.width - 2);
  const int y0 = std::min(static_cast<int>(std::floor(y)), f.height - 2);
  const double fx = x - x0, fy = y - y0;
  const double top = f.at(x0, y0) * (1 - fx) + f.at(x0 + 1, y0) * fx;
  const double bot = f.at(x0, y0 + 1) * (1 - fx) + f.at(x0 + 1, y0 + 1) * fx;
  return top * (1 - fy) + bot * fy;
}

// Median filter that repeats the end values beyond the profile.
std::vector<double> median_baseline(const std::vector<double>& p, int width) {
  const int n = static_cast<int>(p.size());
  const int half = width / 2;
  std::vector<double> out(p.size()), win(static_cast<std::size_t>(2 * half + 1));
  for (int i = 0; i < n; ++i) {
    for (int k = -half; k <= half; ++k) {
      win[static_cast<std::size_t>(k + half)] = p[static_cast<std::size_t>(std::clamp(i + k, 0, n - 1))];
    }
    std::nth_element(win.begin(), win.begin() + half, win.end());
    out[static_cast<std::size_t>(i)] = win[static_cast<std::size_t>(half)];
  }
  return out;
}

}  // namespace

FringeSample measure_fringe(const vision::FrameView& f, const vision::Component& c, int ring_px) {
  FringeSample out;
  if (!f.data || c.touches_mask_edge || ring_px < 1 || f.width < 2 || f.height < 2 || f.pixel_depth == 0) return out;
  std::vector<double> radius;
  if (!outline_radii(c, radius)) return out;

  const int n = 2 * ring_px + 1;
  std::vector<double> profile(static_cast<std::size_t>(n), 0.0);
  int used = 0;
  for (int i = 0; i < kAngles; ++i) {
    const double a = (i + 0.5) / kAngles * 2 * std::numbers::pi - std::numbers::pi;
    const double ca = std::cos(a), sa = std::sin(a);
    const double r0 = radius[static_cast<std::size_t>(i)];
    const double xe1 = c.centroid.x + (r0 - ring_px) * ca, ye1 = c.centroid.y + (r0 - ring_px) * sa;
    const double xe2 = c.centroid.x + (r0 + ring_px) * ca, ye2 = c.centroid.y + (r0 + ring_px) * sa;
    const auto inside = [&](double x, double y) { return x >= 0 && y >= 0 && x <= f.width - 1 && y <= f.height - 1; };
    if (!inside(xe1, ye1) || !inside(xe2, ye2)) continue;
    for (int k = 0; k < n; ++k) {
      const double off = k - ring_px;
      profile[static_cast<std::size_t>(k)] += sample(f, c.centroid.x + (r0 + off) * ca, c.centroid.y + (r0 + off) * sa);
    }
    ++used;
  }
  if (used < kAngles / 2) return out;
  for (double& v : profile) v /= used;

  // Wider than the line, which lifts a median taken over it: the whole
  // profile. A step alone is monotone, and the median of a monotone run is
  // its center, so the step is the baseline and leaves no residual.
  const std::vector<double> base = median_baseline(profile, n);
  std::vector<double> resid(profile.size());
  for (std::size_t k = 0; k < profile.size(); ++k) resid[k] = profile[k] - base[k];

  out.valid = true;
  const auto peak_it = std::max_element(resid.begin(), resid.end());
  const double peak = *peak_it;
  if (peak <= kMinContrastPx) return out;
  out.contrast = peak / f.pixel_depth;

  const int pk = static_cast<int>(peak_it - resid.begin());
  const double floor_v = kPeakShare * peak;
  int lo = pk, hi = pk;
  while (lo > 0 && resid[static_cast<std::size_t>(lo - 1)] >= floor_v) --lo;
  while (hi < n - 1 && resid[static_cast<std::size_t>(hi + 1)] >= floor_v) ++hi;
  double wsum = 0, psum = 0;
  for (int k = lo; k <= hi; ++k) {
    wsum += resid[static_cast<std::size_t>(k)];
    psum += resid[static_cast<std::size_t>(k)] * (k - ring_px);
  }
  out.position_px = psum / wsum;
  return out;
}

double slope_px_s(std::span<const std::pair<TimePoint, double>> samples) {
  if (samples.size() < 2) return 0.0;
  const TimePoint t0 = samples.front().first;
  double st = 0, sv = 0;
  for (const auto& [t, v] : samples) {
    st += std::chrono::duration<double>(t - t0).count();
    sv += v;
  }
  const auto m = static_cast<double>(samples.size());
  const double mt = st / m, mv = sv / m;
  double num = 0, den = 0;
  for (const auto& [t, v] : samples) {
    const double dt = std::chrono::duration<double>(t - t0).count() - mt;
    num += dt * (v - mv);
    den += dt * dt;
  }
  return den > 0 ? num / den : 0.0;
}

}  // namespace pychron::grains
