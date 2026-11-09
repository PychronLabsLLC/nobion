// Records with every field set, for tests that walk a record's fields.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "nobion/earthbank/model.hpp"

namespace nobion::earthbank::testing {

inline void fill(std::optional<double>& field) { field = 1.5; }
inline void fill(std::optional<std::string>& field) { field = "x"; }
inline void fill(std::optional<std::int64_t>& field) { field = 7; }
inline void fill(std::optional<bool>& field) { field = true; }

#define NOBION_EARTHBANK_FILL(kind, name) fill(record.name);
#define NOBION_EARTHBANK_ALL_SET(Record, FIELDS) \
  inline Record all_set(const Record&) {          \
    Record record;                                \
    FIELDS(NOBION_EARTHBANK_FILL)                \
    return record;                                \
  }

NOBION_EARTHBANK_ALL_SET(Sample, NOBION_EARTHBANK_SAMPLE_FIELDS)
NOBION_EARTHBANK_ALL_SET(Location, NOBION_EARTHBANK_LOCATION_FIELDS)
NOBION_EARTHBANK_ALL_SET(DataPoint, NOBION_EARTHBANK_DATA_POINT_FIELDS)
NOBION_EARTHBANK_ALL_SET(Aliquot, NOBION_EARTHBANK_ALIQUOT_FIELDS)
NOBION_EARTHBANK_ALL_SET(Measurement, NOBION_EARTHBANK_MEASUREMENT_FIELDS)
NOBION_EARTHBANK_ALL_SET(AgeCalc, NOBION_EARTHBANK_AGE_CALC_FIELDS)
NOBION_EARTHBANK_ALL_SET(AgeSummary, NOBION_EARTHBANK_AGE_SUMMARY_FIELDS)

#undef NOBION_EARTHBANK_ALL_SET
#undef NOBION_EARTHBANK_FILL

// to_json of a `Record` with every field set: all the keys it can emit.
template <typename Record>
nlohmann::json all_fields() {
  return to_json(all_set(Record{}));
}

}  // namespace nobion::earthbank::testing
