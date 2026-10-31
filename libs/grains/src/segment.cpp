#include "pychron/grains/segment.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pychron::grains {
namespace {

// Peak gradient below this many noise sigma is a flat scene. Noise alone
// reaches about 5 sigma somewhere in a frame of this many pixels.
constexpr double kFlatSigmas = 8.0;
// The threshold is never below this many noise sigma.
constexpr double kNoiseFloorSigmas = 4.0;
// Where a fringe lingers near an edge it lifts the gradient on the skirt of
// the ridge; holding the threshold at this fraction of the strongest edge
// trims the skirt. Low, so a grain much fainter than its neighbours is kept.
constexpr double kSkirtFraction = 0.2;
// Quantile of a pixel's gradient over the window. Low, so a fringe that sits
// on the edge for part of the window does not lift it, but above the minimum.
constexpr double kQuantile = 0.15;
// Median of a Rayleigh magnitude is 1.1774 sigma.
constexpr double kRayleighMedian = 1.1774;
// Gradient magnitudes are scaled to this range for Otsu.
constexpr double kScaleTop = 60000.0;

// A grain cut by the frame edge leaves an arc whose ends meet the border, so
// the interior is not enclosed and is not filled. Joins the ends of each
// component along the border line they meet.
void close_at_border(std::vector<std::uint8_t>& mask, int w, int h, const std::vector<vision::Component>& found) {
  auto at = [&](int x, int y) -> std::uint8_t& { return mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)]; };
  for (const vision::Component& c : found) {
    // Extent of the component's boundary on each of the four border lines.
    int top_lo = w, top_hi = -1, bottom_lo = w, bottom_hi = -1;
    int left_lo = h, left_hi = -1, right_lo = h, right_hi = -1;
    for (const vision::Vec2& p : c.boundary) {
      const int x = static_cast<int>(std::lround(p.x)), y = static_cast<int>(std::lround(p.y));
      if (y == 0) top_lo = std::min(top_lo, x), top_hi = std::max(top_hi, x);
      if (y == h - 1) bottom_lo = std::min(bottom_lo, x), bottom_hi = std::max(bottom_hi, x);
      if (x == 0) left_lo = std::min(left_lo, y), left_hi = std::max(left_hi, y);
      if (x == w - 1) right_lo = std::min(right_lo, y), right_hi = std::max(right_hi, y);
    }
    for (int x = top_lo; x <= top_hi; ++x) at(x, 0) = 1;
    for (int x = bottom_lo; x <= bottom_hi; ++x) at(x, h - 1) = 1;
    for (int y = left_lo; y <= left_hi; ++y) at(0, y) = 1;
    for (int y = right_lo; y <= right_hi; ++y) at(w - 1, y) = 1;
  }
}

// Fills every region the edge pixels enclose, then takes one pixel off the
// outside: the Sobel ridge reaches a pixel or so past the edge it marks.
void fill_and_erode(std::vector<std::uint8_t>& mask, int w, int h) {
  const auto uw = static_cast<std::size_t>(w);
  auto idx = [&](int x, int y) { return static_cast<std::size_t>(y) * uw + static_cast<std::size_t>(x); };
  std::vector<std::uint8_t> outside(mask.size(), 0);
  std::vector<int> stack;
  auto seed = [&](int x, int y) {
    if (mask[idx(x, y)] == 0 && outside[idx(x, y)] == 0) {
      outside[idx(x, y)] = 1;
      stack.push_back(y * w + x);
    }
  };
  for (int x = 0; x < w; ++x) seed(x, 0), seed(x, h - 1);
  for (int y = 0; y < h; ++y) seed(0, y), seed(w - 1, y);
  while (!stack.empty()) {
    const int at = stack.back();
    stack.pop_back();
    const int x = at % w, y = at / w;
    if (x > 0) seed(x - 1, y);
    if (x < w - 1) seed(x + 1, y);
    if (y > 0) seed(x, y - 1);
    if (y < h - 1) seed(x, y + 1);
  }
  std::vector<std::uint8_t> filled(mask.size());
  for (std::size_t i = 0; i < filled.size(); ++i) filled[i] = outside[i] == 0 ? 1 : 0;
  // The frame border counts as foreground for the erosion: a grain cut by it
  // stays cut by it.
  auto on = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h || filled[idx(x, y)] != 0; };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      mask[idx(x, y)] = filled[idx(x, y)] != 0 && on(x - 1, y) && on(x + 1, y) && on(x, y - 1) && on(x, y + 1) ? 1 : 0;
    }
  }
}

}  // namespace

Segmenter::Segmenter(const GrainOptions& options) : options_(options) {}

void Segmenter::reset() {
  ring_.clear();
  width_ = height_ = 0;
  depth_ = 0;
}

std::vector<vision::Component> Segmenter::update(const vision::FrameView& v) {
  if (v.data == nullptr || v.width < 3 || v.height < 3) return {};
  if (v.width != width_ || v.height != height_ || v.pixel_depth != depth_) {
    reset();
    width_ = v.width;
    height_ = v.height;
    depth_ = v.pixel_depth;
  }
  const auto w = static_cast<std::size_t>(width_);
  const auto h = static_cast<std::size_t>(height_);

  // Sobel magnitude of this frame, in pixel levels per pixel.
  std::vector<float> mag(w * h, 0.0F);
  for (int y = 1; y < height_ - 1; ++y) {
    for (int x = 1; x < width_ - 1; ++x) {
      auto p = [&](int dx, int dy) { return static_cast<double>(v.at(x + dx, y + dy)); };
      const double gx = (p(1, -1) + 2 * p(1, 0) + p(1, 1)) - (p(-1, -1) + 2 * p(-1, 0) + p(-1, 1));
      const double gy = (p(-1, 1) + 2 * p(0, 1) + p(1, 1)) - (p(-1, -1) + 2 * p(0, -1) + p(1, -1));
      mag[static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)] = static_cast<float>(std::hypot(gx, gy));
    }
  }

  // Noise: the median of a gradient magnitude over a mostly flat frame.
  std::vector<float> sorted(mag);
  const auto mid = sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2);
  std::nth_element(sorted.begin(), mid, sorted.end());
  const double sigma = static_cast<double>(*mid) / kRayleighMedian;

  ring_.push_back(std::move(mag));
  const auto window = static_cast<std::size_t>(std::max(options_.segment_window, 1));
  while (ring_.size() > window) ring_.pop_front();

  // Per pixel, the kQuantile of the magnitudes over the ring.
  const auto rank = static_cast<std::size_t>(kQuantile * static_cast<double>(ring_.size() - 1));
  std::vector<float> stable(w * h);
  std::vector<float> column(ring_.size());
  for (std::size_t i = 0; i < stable.size(); ++i) {
    for (std::size_t k = 0; k < ring_.size(); ++k) column[k] = ring_[k][i];
    const auto nth = column.begin() + static_cast<std::ptrdiff_t>(rank);
    std::nth_element(column.begin(), nth, column.end());
    stable[i] = *nth;
  }

  const double peak = static_cast<double>(*std::max_element(stable.begin(), stable.end()));
  if (peak <= 0 || peak < kFlatSigmas * sigma) return {};

  const double scale = kScaleTop / peak;
  vision::Frame g = vision::Frame::make(width_, height_, 65535);
  for (std::size_t i = 0; i < stable.size(); ++i) {
    g.data[i] = static_cast<std::uint16_t>(static_cast<double>(stable[i]) * scale);
  }
  // Otsu alone can land inside the noise floor when edges are a tiny part of
  // the frame, so it is held above a few sigma.
  const auto floor_level = static_cast<std::uint16_t>(std::min(kNoiseFloorSigmas * sigma * scale, kScaleTop));
  const auto skirt_level = static_cast<std::uint16_t>(kSkirtFraction * kScaleTop);
  const std::uint16_t threshold = std::max({vision::otsu(g.view(), 0.0), floor_level, skirt_level});

  std::vector<std::uint8_t> mask(w * h, 0);
  for (std::size_t i = 0; i < mask.size(); ++i) mask[i] = g.data[i] > threshold ? 1 : 0;

  // Sobel leaves the outermost pixels at zero; carry the inner neighbour out.
  for (int x = 0; x < width_; ++x) {
    mask[static_cast<std::size_t>(x)] = mask[w + static_cast<std::size_t>(x)];
    mask[(h - 1) * w + static_cast<std::size_t>(x)] = mask[(h - 2) * w + static_cast<std::size_t>(x)];
  }
  for (int y = 0; y < height_; ++y) {
    mask[static_cast<std::size_t>(y) * w] = mask[static_cast<std::size_t>(y) * w + 1];
    mask[static_cast<std::size_t>(y) * w + w - 1] = mask[static_cast<std::size_t>(y) * w + w - 2];
  }

  const std::vector<vision::Component> rings = vision::components(mask, width_, height_, 0.0);
  close_at_border(mask, width_, height_, rings);
  fill_and_erode(mask, width_, height_);
  std::vector<vision::Component> found = vision::components(mask, width_, height_, 0.0);
  std::erase_if(found, [&](const vision::Component& c) { return c.area < options_.min_area_px; });
  return found;
}

}  // namespace pychron::grains
