#include "pychron/grains/shift.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace pychron::grains {
namespace {

constexpr int kBox = 4;
constexpr int kRefine = kBox - 1;

struct Plane {
  int w = 0, h = 0;
  std::vector<float> v;
  float at(int x, int y) const { return v[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; }
};

Plane full(const vision::FrameView& f) {
  Plane p{f.width, f.height, std::vector<float>(static_cast<std::size_t>(f.width) * static_cast<std::size_t>(f.height))};
  for (int y = 0; y < f.height; ++y) {
    for (int x = 0; x < f.width; ++x) {
      p.v[static_cast<std::size_t>(y) * static_cast<std::size_t>(f.width) + static_cast<std::size_t>(x)] = f.at(x, y);
    }
  }
  return p;
}

Plane box_down(const Plane& s) {
  Plane d{s.w / kBox, s.h / kBox, {}};
  d.v.assign(static_cast<std::size_t>(d.w) * static_cast<std::size_t>(d.h), 0.0F);
  for (int y = 0; y < d.h; ++y) {
    for (int x = 0; x < d.w; ++x) {
      float sum = 0;
      for (int j = 0; j < kBox; ++j) {
        for (int i = 0; i < kBox; ++i) sum += s.at(x * kBox + i, y * kBox + j);
      }
      d.v[static_cast<std::size_t>(y) * static_cast<std::size_t>(d.w) + static_cast<std::size_t>(x)] = sum / (kBox * kBox);
    }
  }
  return d;
}

// Mean absolute difference of cur(p + d) against prev(p) over the overlap,
// sampling every `step` pixels. Infinite when the overlap is too small.
double sad(const Plane& prev, const Plane& cur, int dx, int dy, int step) {
  const int x0 = std::max(0, -dx), x1 = std::min(prev.w, cur.w - dx);
  const int y0 = std::max(0, -dy), y1 = std::min(prev.h, cur.h - dy);
  if (x1 - x0 < prev.w / 2 || y1 - y0 < prev.h / 2) return std::numeric_limits<double>::infinity();
  double sum = 0;
  long n = 0;
  for (int y = y0; y < y1; y += step) {
    for (int x = x0; x < x1; x += step) {
      sum += std::abs(cur.at(x + dx, y + dy) - prev.at(x, y));
      ++n;
    }
  }
  return sum / static_cast<double>(n);
}

}  // namespace

vision::Vec2 estimate_shift(const vision::FrameView& prev, const vision::FrameView& cur, int search_px) {
  if (!prev.data || !cur.data || prev.width < 2 * kBox || prev.height < 2 * kBox || search_px <= 0) return {};
  const Plane p = full(prev), c = full(cur);
  const Plane pd = box_down(p), cd = box_down(c);

  // Coarse: every shift of the downsampled frames within the search range.
  const int reach = (search_px + kBox - 1) / kBox;
  double best = std::numeric_limits<double>::infinity();
  int bx = 0, by = 0;
  for (int dy = -reach; dy <= reach; ++dy) {
    for (int dx = -reach; dx <= reach; ++dx) {
      const double s = sad(pd, cd, dx, dy, 1);
      // Ties keep the smaller shift: scan order is not a preference.
      if (s < best || (s == best && std::abs(dx) + std::abs(dy) < std::abs(bx) + std::abs(by))) {
        best = s;
        bx = dx;
        by = dy;
      }
    }
  }
  if (!std::isfinite(best)) return {};

  // Fine: full resolution about the coarse winner, to the pixel.
  double fine = std::numeric_limits<double>::infinity();
  int fx = bx * kBox, fy = by * kBox;
  const int cx = fx, cy = fy;
  for (int dy = cy - kRefine; dy <= cy + kRefine; ++dy) {
    for (int dx = cx - kRefine; dx <= cx + kRefine; ++dx) {
      if (std::abs(dx) > search_px || std::abs(dy) > search_px) continue;
      const double s = sad(p, c, dx, dy, 2);
      if (s < fine || (s == fine && std::abs(dx) + std::abs(dy) < std::abs(fx) + std::abs(fy))) {
        fine = s;
        fx = dx;
        fy = dy;
      }
    }
  }
  if (!std::isfinite(fine)) return {};
  return {static_cast<double>(fx), static_cast<double>(fy)};
}

}  // namespace pychron::grains
