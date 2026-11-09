#pragma once

// The store and entry headers, for UI sources. Qt's `signals` keyword macro
// would rewrite persistence::CollectionRoots::signals, so it is set aside
// while they are read.

#pragma push_macro("signals")
#undef signals
#include "nobion/entry/csv.hpp"
#include "nobion/entry/export.hpp"
#include "nobion/entry/holder_import.hpp"
#include "nobion/entry/identifier_plan.hpp"
#include "nobion/entry/level_sheet.hpp"
#include "nobion/entry/names.hpp"
#include "nobion/entry/package_edit.hpp"
#include "nobion/entry/positions_import.hpp"
#include "nobion/entry/sample_fields.hpp"
#include "nobion/entry/sample_import.hpp"
#include "nobion/entry/sample_search.hpp"
#include "nobion/entry/settings.hpp"
#include "nobion/persistence/store.hpp"
#pragma pop_macro("signals")
