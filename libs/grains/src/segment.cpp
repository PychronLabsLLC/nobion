#include "pychron/grains/segment.hpp"

#include <algorithm>
#include <array>
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
// the ridge; a pixel below this fraction of the strongest ridge within
// kSkirtReachPx of it is trimmed.
constexpr double kSkirtFraction = 0.2;
constexpr int kSkirtReachPx = 8;
// Otsu's level is held at or below this fraction of the nearby ridge.
constexpr double kOtsuCap = 0.5;
// Quantile of a pixel's level over the window. Low, since a line only adds
// light, but above the minimum, which noise alone would pull down.
constexpr double kQuantile = 0.15;
// Segmenting works on a frame at most this wide.
constexpr int kMaxWidthPx = 800;
// Nothing is returned until the ring holds this many frames; a quantile
// rank of at least 1 needs it, and it is the window's lower bound.
constexpr int kWarmUpFrames = 8;
// Components are computed every this many frames; between, the last result.
constexpr int kComputeEvery = 3;
// The noise median looks at every this many pixels.
constexpr std::size_t kSampleStride = 7;
// Largest window the per-pixel scratch holds.
constexpr std::size_t kMaxWindow = 64;
// Median of a Rayleigh magnitude is 1.1774 sigma.
constexpr double kRayleighMedian = 1.1774;
// Gradient magnitudes are scaled to this range for Otsu.
constexpr double kScaleTop = 60000.0;

// Maximum over the (2r+1) square around each pixel, as two passes.
std::vector<std::uint16_t> local_max(const std::vector<std::uint16_t>& v, int w, int h, int r) {
  const auto uw = static_cast<std::size_t>(w);
  const auto uh = static_cast<std::size_t>(h);
  std::vector<std::uint16_t> rows(v.size());
  for (std::size_t y = 0; y < uh; ++y) {
    for (int x = 0; x < w; ++x) {
      const int lo = std::max(0, x - r), hi = std::min(w - 1, x + r);
      std::uint16_t m = 0;
      for (int k = lo; k <= hi; ++k) m = std::max(m, v[y * uw + static_cast<std::size_t>(k)]);
      rows[y * uw + static_cast<std::size_t>(x)] = m;
    }
  }
  std::vector<std::uint16_t> out(v.size());
  for (int y = 0; y < h; ++y) {
    const int lo = std::max(0, y - r), hi = std::min(h - 1, y + r);
    for (std::size_t x = 0; x < uw; ++x) {
      std::uint16_t m = 0;
      for (int k = lo; k <= hi; ++k) m = std::max(m, rows[static_cast<std::size_t>(k) * uw + x]);
      out[static_cast<std::size_t>(y) * uw + x] = m;
    }
  }
  return out;
}

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
  buffer_.clear();
  cached_.clear();
  width_ = height_ = 0;
  depth_ = 0;
  factor_ = 1;
  slot_ = 0;
  filled_ = 0;
  since_ = 0;
  computed_ = false;
}

std::vector<vision::Component> Segmenter::update(const vision::FrameView& v) {
  if (v.data == nullptr || v.width < 3 || v.height < 3) return {};
  if (v.width != width_ || v.height != height_ || v.pixel_depth != depth_) {
    reset();
    width_ = v.width;
    height_ = v.height;
    depth_ = v.pixel_depth;
    factor_ = (v.width + kMaxWidthPx - 1) / kMaxWidthPx;
    sw_ = v.width / factor_;
    sh_ = v.height / factor_;
    if (sw_ < 3 || sh_ < 3) {
      factor_ = 1;
      sw_ = v.width;
      sh_ = v.height;
    }
    window_ = static_cast<std::size_t>(std::min<std::size_t>(static_cast<std::size_t>(std::max(options_.segment_window, kWarmUpFrames)), kMaxWindow));
    const auto n = static_cast<std::size_t>(sw_) * static_cast<std::size_t>(sh_);
    buffer_.assign(n * window_, 0.0F);
    small_.assign(n, 0.0F);
  }
  const auto w = static_cast<std::size_t>(sw_);
  const auto h = static_cast<std::size_t>(sh_);
  const auto f = static_cast<std::size_t>(factor_);

  // Box mean down to at most kMaxWidthPx wide.
  const float norm = 1.0F / static_cast<float>(factor_ * factor_);
  for (std::size_t y = 0; y < h; ++y) {
    for (std::size_t x = 0; x < w; ++x) {
      std::uint32_t sum = 0;
      for (std::size_t dy = 0; dy < f; ++dy) {
        const std::uint16_t* row = v.data + (y * f + dy) * static_cast<std::size_t>(v.stride) + x * f;
        for (std::size_t dx = 0; dx < f; ++dx) sum += row[dx];
      }
      small_[y * w + x] = static_cast<float>(sum) * norm;
    }
  }

  // This frame into the ring, pixel-major so a pixel's history is contiguous.
  for (std::size_t i = 0; i < w * h; ++i) buffer_[i * window_ + slot_] = small_[i];
  slot_ = (slot_ + 1) % window_;
  filled_ = std::min(filled_ + 1, window_);
  ++since_;

  if (filled_ < static_cast<std::size_t>(kWarmUpFrames)) return {};
  if (since_ < kComputeEvery && computed_) return cached_;
  since_ = 0;
  computed_ = true;

  // Per pixel, the kQuantile of the levels over the ring.
  const auto rank = static_cast<std::ptrdiff_t>(kQuantile * static_cast<double>(filled_ - 1));
  std::vector<float> quiet(w * h);
  std::array<float, kMaxWindow> column{};
  for (std::size_t i = 0; i < quiet.size(); ++i) {
    const float* src = &buffer_[i * window_];
    std::copy(src, src + filled_, column.begin());
    const auto nth = column.begin() + rank;
    std::nth_element(column.begin(), nth, column.begin() + static_cast<std::ptrdiff_t>(filled_));
    quiet[i] = *nth;
  }

  // Its Sobel magnitude; the outermost pixels stay zero.
  std::vector<float> stable(w * h, 0.0F);
  for (std::size_t y = 1; y + 1 < h; ++y) {
    for (std::size_t x = 1; x + 1 < w; ++x) {
      const float* r0 = &quiet[(y - 1) * w + x];
      const float* r1 = &quiet[y * w + x];
      const float* r2 = &quiet[(y + 1) * w + x];
      const float gx = (r0[1] + 2 * r1[1] + r2[1]) - (r0[-1] + 2 * r1[-1] + r2[-1]);
      const float gy = (r2[-1] + 2 * r2[0] + r2[1]) - (r0[-1] + 2 * r0[0] + r0[1]);
      stable[y * w + x] = std::sqrt(gx * gx + gy * gy);
    }
  }

  // Noise: the median gradient magnitude over a strided sample.
  std::vector<float> sample;
  sample.reserve(w * h / kSampleStride + 1);
  for (std::size_t i = 0; i < w * h; i += kSampleStride) sample.push_back(stable[i]);
  const auto mid = sample.begin() + static_cast<std::ptrdiff_t>(sample.size() / 2);
  std::nth_element(sample.begin(), mid, sample.end());
  const double sigma = static_cast<double>(*mid) / kRayleighMedian;

  cached_ = components_of(stable, sigma);
  return cached_;
}

std::vector<vision::Component> Segmenter::components_of(const std::vector<float>& stable, double sigma) const {
  const auto w = static_cast<std::size_t>(sw_);
  const auto h = static_cast<std::size_t>(sh_);
  const double peak = static_cast<double>(*std::max_element(stable.begin(), stable.end()));
  if (peak <= 0 || peak < kFlatSigmas * sigma) return {};

  const double scale = kScaleTop / peak;
  vision::Frame g = vision::Frame::make(sw_, sh_, 65535);
  for (std::size_t i = 0; i < stable.size(); ++i) {
    g.data[i] = static_cast<std::uint16_t>(static_cast<double>(stable[i]) * scale);
  }
  // Otsu alone can land inside the noise floor when edges are a tiny part of
  // the frame, so it is held above a few sigma.
  const auto floor_level = static_cast<std::uint16_t>(std::min(kNoiseFloorSigmas * sigma * scale, kScaleTop));
  const std::uint16_t otsu = vision::otsu(g.view(), 0.0);

  // Otsu is a global level, and a strong ridge elsewhere (or a fringe on the
  // edge in some frames) can put it above a faint grain's edge. So it is
  // capped at half the strongest ridge within kSkirtReachPx, and the skirt trim
  // is relative to that ridge too.
  const std::vector<std::uint16_t> nearby = local_max(g.data, sw_, sh_, kSkirtReachPx);
  std::vector<std::uint8_t> mask(w * h, 0);
  for (std::size_t i = 0; i < mask.size(); ++i) {
    const auto ridge = static_cast<double>(nearby[i]);
    const double level = std::max(static_cast<double>(floor_level), std::min(static_cast<double>(otsu), kOtsuCap * ridge));
    mask[i] = static_cast<double>(g.data[i]) > level && static_cast<double>(g.data[i]) > kSkirtFraction * ridge ? 1 : 0;
  }

  // Sobel leaves the outermost pixels at zero; carry the inner neighbour out.
  for (std::size_t x = 0; x < w; ++x) {
    mask[x] = mask[w + x];
    mask[(h - 1) * w + x] = mask[(h - 2) * w + x];
  }
  for (std::size_t y = 0; y < h; ++y) {
    mask[y * w] = mask[y * w + 1];
    mask[y * w + w - 1] = mask[y * w + w - 2];
  }

  const std::vector<vision::Component> rings = vision::components(mask, sw_, sh_, 0.0);
  close_at_border(mask, sw_, sh_, rings);
  fill_and_erode(mask, sw_, sh_);
  std::vector<vision::Component> found = vision::components(mask, sw_, sh_, 0.0);

  // Back to frame pixels: a downscaled pixel i covers i*f .. i*f + f - 1.
  const double f = factor_;
  const double half = (f - 1.0) / 2.0;
  const double min_area = options_.min_area_px / (f * f);
  std::erase_if(found, [&](const vision::Component& c) { return c.area < min_area; });
  for (vision::Component& c : found) {
    c.area *= f * f;
    c.perimeter *= f;
    c.centroid = {c.centroid.x * f + half, c.centroid.y * f + half};
    c.bbox = {c.bbox.x * factor_, c.bbox.y * factor_, c.bbox.w * factor_, c.bbox.h * factor_};
    for (vision::Vec2& p : c.boundary) p = {p.x * f + half, p.y * f + half};
  }
  return found;
}

}  // namespace pychron::grains
