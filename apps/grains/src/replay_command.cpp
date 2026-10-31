#include "replay_command.hpp"

#include <ostream>

#include "pychron/grains/recording.hpp"

namespace pychron::grains {

int run_replay(const std::filesystem::path& folder, std::ostream& out, std::ostream& err) {
  const auto summary = replay_folder(folder);
  if (!summary) {
    err << "grains replay: " << summary.error().what << '\n';
    return kReplayFailed;
  }
  out << summary_text(*summary);
  if (summary->mismatches > 0) {
    err << "grains replay: " << summary->mismatches << " of " << summary->frames << " frames differ from the recording\n";
    return kReplayMismatched;
  }
  return 0;
}

}  // namespace pychron::grains
