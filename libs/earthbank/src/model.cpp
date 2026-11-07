#include "nobion/earthbank/model.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nobion/core/sha256.hpp"

namespace nobion::earthbank {
namespace {

template <typename T>
void put(nlohmann::json& out, const char* name, const std::optional<T>& field) {
  if (field) out[name] = *field;
}

// A number that is not finite has no JSON form; it is not sent.
void put(nlohmann::json& out, const char* name, const Number& field) {
  if (field && std::isfinite(*field)) out[name] = *field;
}

// An empty string tells the service nothing; it is not sent.
void put(nlohmann::json& out, const char* name, const Text& field) {
  if (field && !field->empty()) out[name] = *field;
}

template <typename Record>
nlohmann::json array_of(const std::vector<Record>& records) {
  nlohmann::json out = nlohmann::json::array();
  for (const Record& record : records) out.push_back(to_json(record));
  return out;
}

}  // namespace

#define NOBION_EARTHBANK_PUT(kind, name) put(out, #name, record.name);
#define NOBION_EARTHBANK_TO_JSON(Record, FIELDS)   \
  nlohmann::json to_json(const Record& record) {    \
    nlohmann::json out = nlohmann::json::object();  \
    FIELDS(NOBION_EARTHBANK_PUT)                   \
    return out;                                     \
  }

NOBION_EARTHBANK_TO_JSON(Sample, NOBION_EARTHBANK_SAMPLE_FIELDS)
NOBION_EARTHBANK_TO_JSON(Location, NOBION_EARTHBANK_LOCATION_FIELDS)
NOBION_EARTHBANK_TO_JSON(DataPoint, NOBION_EARTHBANK_DATA_POINT_FIELDS)
NOBION_EARTHBANK_TO_JSON(Aliquot, NOBION_EARTHBANK_ALIQUOT_FIELDS)
NOBION_EARTHBANK_TO_JSON(Measurement, NOBION_EARTHBANK_MEASUREMENT_FIELDS)
NOBION_EARTHBANK_TO_JSON(AgeCalc, NOBION_EARTHBANK_AGE_CALC_FIELDS)
NOBION_EARTHBANK_TO_JSON(AgeSummary, NOBION_EARTHBANK_AGE_SUMMARY_FIELDS)

#undef NOBION_EARTHBANK_TO_JSON
#undef NOBION_EARTHBANK_PUT

std::string content_hash(const Submission& submission) {
  nlohmann::json records = nlohmann::json::array();
  records.push_back(submission.datapoint_name);
  records.push_back(submission.setup_name);
  records.push_back(submission.irradiation_name);
  records.push_back(to_json(submission.sample));
  records.push_back(submission.location ? to_json(*submission.location) : nlohmann::json(nullptr));
  records.push_back(to_json(submission.data_point));
  records.push_back(array_of(submission.aliquots));
  records.push_back(array_of(submission.measurements));
  records.push_back(to_json(submission.age_calc));
  records.push_back(to_json(submission.age_summary));
  const Sha256Digest digest = sha256(records.dump());
  return to_hex(std::span<const std::uint8_t>(digest));
}

}  // namespace nobion::earthbank
