#pragma once

// A release's analyses for a destination that wants them rather than a
// directory (publication workflow design, section 5.2): the members at the
// revisions the release's bookmark pins, in member order, unreduced.

#include <vector>

#include "nobion/core/error.hpp"
#include "nobion/persistence/ids.hpp"
#include "nobion/persistence/store.hpp"
#include "nobion/processing/model.hpp"
#include "nobion/processing/store_source.hpp"

namespace pychron::publish {

// Every member of `release`, embargoed ones included (the caller decides what
// may leave the lab), in member order (uuid order). Config error when there is no such
// release.
Result<std::vector<processing::AnalysisPtr>> release_dataset(persistence::IStore& store,
                                                             processing::StoreSource& source,
                                                             persistence::Uuid release);

}  // namespace pychron::publish
