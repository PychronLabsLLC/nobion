#pragma once

#include <filesystem>
#include <iosfwd>

namespace pychron::grains {

inline constexpr int kReplayFailed = 1;      // folder missing, or its files do not parse or match
inline constexpr int kReplayMismatched = 3;  // ran, but a report differs from the recorded one

// `grains replay <folder>`: runs the recording again and prints its summary.
// 0 only when every frame's report is the one recorded.
[[nodiscard]] int run_replay(const std::filesystem::path& folder, std::ostream& out, std::ostream& err);

}  // namespace pychron::grains
