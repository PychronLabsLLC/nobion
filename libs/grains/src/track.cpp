#include "pychron/grains/track.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace pychron::grains {

namespace {

constexpr double kMinIou = 0.3;

double iou(const vision::Rect& a, const vision::Rect& b) {
  const int x0 = std::max(a.x, b.x);
  const int y0 = std::max(a.y, b.y);
  const int x1 = std::min(a.x + a.w, b.x + b.w);
  const int y1 = std::min(a.y + a.h, b.y + b.h);
  if (x1 <= x0 || y1 <= y0) return 0.0;
  const double inter = static_cast<double>(x1 - x0) * (y1 - y0);
  const double uni = static_cast<double>(a.w) * a.h + static_cast<double>(b.w) * b.h - inter;
  return uni > 0.0 ? inter / uni : 0.0;
}

}  // namespace

Tracker::Tracker(const GrainOptions& options) : options_(options) {}

const std::vector<Track>& Tracker::update(std::vector<vision::Component> blobs) {
  const std::size_t nt = tracks_.size();
  const std::size_t nb = blobs.size();

  std::vector<int> track_links(nt, 0);
  std::vector<int> blob_links(nb, 0);
  std::vector<std::pair<std::size_t, std::size_t>> links;
  for (std::size_t t = 0; t < nt; ++t) {
    for (std::size_t b = 0; b < nb; ++b) {
      if (iou(tracks_[t].blob.bbox, blobs[b].bbox) >= kMinIou) {
        links.emplace_back(t, b);
        ++track_links[t];
        ++blob_links[b];
      }
    }
  }

  // A link is a match only when neither end has another. Every other link
  // is a merge or a split, and both its ends are cut loose: the track ends
  // and the blob is new.
  std::vector<int> match(nt, -1);
  std::vector<bool> track_cut(nt, false);
  std::vector<bool> blob_cut(nb, false);
  for (const auto& link : links) {
    if (track_links[link.first] == 1 && blob_links[link.second] == 1) {
      match[link.first] = static_cast<int>(link.second);
    } else {
      track_cut[link.first] = true;
      blob_cut[link.second] = true;
    }
  }
  std::vector<Track> next;
  next.reserve(nt + nb);
  std::vector<bool> blob_used(nb, false);
  for (std::size_t t = 0; t < nt; ++t) {
    Track& track = tracks_[t];
    if (!track_cut[t] && match[t] >= 0) {
      const auto b = static_cast<std::size_t>(match[t]);
      track.blob = std::move(blobs[b]);
      track.missed = 0;
      blob_used[b] = true;
      next.push_back(std::move(track));
    } else if (!track_cut[t] && ++track.missed <= options_.track_gap_frames) {
      next.push_back(std::move(track));
    } else {
      ended_.push_back(track.id);
    }
  }
  for (std::size_t b = 0; b < nb; ++b) {
    if (blob_used[b]) continue;
    Track track;
    track.id = next_id_++;
    track.blob = std::move(blobs[b]);
    next.push_back(std::move(track));
  }
  tracks_ = std::move(next);
  return tracks_;
}

std::vector<std::uint64_t> Tracker::take_ended() { return std::exchange(ended_, {}); }

void Tracker::reset() {
  tracks_.clear();
  ended_.clear();
  next_id_ = 1;
}

}  // namespace pychron::grains
