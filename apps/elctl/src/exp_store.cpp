// elctl exp run's database, with persistence: an experiment::store::Acquisition.

#include "exp_store.hpp"

#include <utility>

#include "nobion/core/build_info.hpp"
#include "nobion/experiment/store/store_persister.hpp"

namespace elctl {

namespace {

class AcquisitionStore final : public ExpStore {
 public:
  explicit AcquisitionStore(std::unique_ptr<nobion::experiment::store::Acquisition> acquisition)
      : acquisition_(std::move(acquisition)) {}
  nobion::experiment::persist::IAnalysisPersister& persister() override { return acquisition_->persister(); }

 private:
  std::unique_ptr<nobion::experiment::store::Acquisition> acquisition_;
};

}  // namespace

nobion::Result<std::unique_ptr<ExpStore>> open_exp_store(const std::string& url, const std::filesystem::path& data) {
  auto opened = nobion::experiment::store::open_acquisition(url, data, "nobion " + nobion::build_id());
  if (!opened) return nobion::fail(std::move(opened).error());
  return std::unique_ptr<ExpStore>(std::make_unique<AcquisitionStore>(std::move(*opened)));
}

bool saves_simulated_runs(const std::string& url, bool from_db_option, bool sim_save) {
  return nobion::experiment::store::saves_simulated_runs(url, from_db_option, sim_save);
}

std::string show_database(const std::string& url) { return nobion::experiment::store::without_password(url); }

}  // namespace elctl
