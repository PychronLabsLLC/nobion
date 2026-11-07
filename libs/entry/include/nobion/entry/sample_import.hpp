#pragma once

// Bulk sample entry from CSV or pasted spreadsheet rows (sample and package
// entry spec, sections 6 and 9.2). Planning is pure: it reads a snapshot of
// the catalog and returns, per row, what an import would do. The plan becomes
// one catalog edit batch.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nobion/entry/csv.hpp"
#include "nobion/entry/names.hpp"
#include "nobion/persistence/store.hpp"

namespace nobion::entry {

enum class ImportField {
  Sample,
  Project,
  PrincipalInvestigator,
  PrincipalInvestigatorKind,   // "person" (default) or "organisation"
  PrincipalInvestigatorOrcid,  // the PI with this ORCID, whatever the name cell says
  PrincipalInvestigatorUuid,   // the PI itself, no matching (written by the sheet export)
  Material,
  Grainsize,
  Note,
  Igsn,
  Lat,
  Lon,
  Elevation,
  StorageLocation,
  Location,
  Unit,
  Lithology,
  LithologyClass,
  LithologyType,
  LithologyGroup,
  ApproximateAge,
  Easting,
  Northing,
  Zone
};

// Every field, in template order.
const std::vector<ImportField>& import_fields();
// The template header name ("sample", "principal_investigator", ...).
std::string_view field_name(ImportField field);
// The field a header names: the template name or one of its aliases
// ("latitude", "pi", "grain_size", ...), case and spaces ignored.
std::optional<ImportField> field_for_header(std::string_view header);

// Which field each column holds; nullopt: ignored.
using ColumnMapping = std::vector<std::optional<ImportField>>;
ColumnMapping default_mapping(const std::vector<std::string>& header);

// The catalog rows a plan is checked against.
struct CatalogSnapshot {
  std::vector<persistence::PrincipalInvestigatorRow> principal_investigators;
  std::vector<persistence::ProjectRow> projects;
  std::vector<persistence::MaterialRow> materials;
  std::vector<persistence::SampleRow> samples;
};
Result<CatalogSnapshot> read_snapshot(persistence::IStore& store);

// Ask: the row's PI has namesakes and the sheet does not say which it is
// (PI spec 6); the row waits for SampleImportPlan::answer.
enum class RowState { Create, Exists, Update, Error, Ask };
std::string_view to_string(RowState state);

struct ImportRow {
  int line = 0;
  RowState state = RowState::Error;
  std::string sample, project, principal_investigator, material, grainsize;
  PiName pi;                              // the PI as the row names it
  std::optional<std::string> pi_orcid;    // checked (names.hpp parse_orcid)
  std::optional<persistence::Uuid> pi_uuid;  // the stored PI, when the row names one beyond doubt
  bool pi_asked = false;                  // the PI is a question of the plan's (by question_name(pi))
  bool well_formed = false;               // no error of its own; may still be a duplicate once its PI is settled
  persistence::SampleFields fields;     // what the row gives
  std::optional<persistence::Uuid> existing;  // the sample with this natural key
  std::vector<std::string> changed;     // Update: the columns that differ
  std::vector<std::string> messages;    // Error: every problem; otherwise notes
};

struct ImportOptions {
  bool update_existing = false;  // write Update rows; otherwise they are left alone
};

// What a person answers about a PI with namesakes: one of the candidates, or
// nullopt for another person, made new.
struct PiAnswer {
  std::optional<persistence::Uuid> pi;
  friend bool operator==(const PiAnswer&, const PiAnswer&) = default;
};

// A PI named in the sheet by name alone (no ORCID, no uuid) whose family name
// and given initial match stored PIs, none of them exactly one by that name:
// asked once for every row that names it (PI spec 6).
struct PiQuestion {
  std::string name;  // question_name(): "Family, Given", or "Name (organisation)"
  PiName pi;
  std::vector<int> lines;
  std::vector<persistence::PrincipalInvestigatorRow> candidates;  // in catalog order
  std::optional<PiAnswer> answer;
};

// The text a question is asked under: the sheet's "Family, Given" of a
// person, an organisation's name followed by " (organisation)".
std::string question_name(const PiName& pi);

struct SampleImportPlan {
  std::vector<ImportRow> rows;
  int creates = 0, exists = 0, updates = 0, errors = 0, asks = 0;
  // Rows that would be created along the way, as "Family, Given", "project (PI)", "material (grainsize)".
  std::vector<std::string> new_principal_investigators, new_projects, new_materials;
  std::vector<PiQuestion> questions;
  bool update_existing = false;  // ImportOptions::update_existing the plan was made with

  // Every question has an answer: only then can the plan be applied.
  [[nodiscard]] bool ready() const;
  // Answers the question `name` (PiQuestion::name) for every row of it and
  // settles those rows against `catalog` (the snapshot the plan was made
  // from). An unknown name, or a PI that is not one of the candidates, is an
  // error and changes nothing.
  Result<void> answer(std::string_view name, PiAnswer answer, const CatalogSnapshot& catalog);
};

SampleImportPlan plan_sample_import(const CsvTable& table, const ColumnMapping& mapping,
                                    const CatalogSnapshot& catalog, const ImportOptions& options);

// Inserts in foreign-key order (PIs, projects, materials, samples) for every
// Create row, and updates (with the snapshot values as expected) for Update
// rows when options.update_existing. Error rows are left out. A plan with a
// question not answered is refused.
Result<persistence::CatalogEditBatch> to_batch(const SampleImportPlan& plan, const CatalogSnapshot& catalog,
                                       const ImportOptions& options);

// The sample columns of `fields`, every one (null where the field is unset).
persistence::CatalogFields sample_columns(const persistence::SampleFields& fields);
// The inverse; columns that are not sample fields are ignored.
persistence::SampleFields sample_fields_of(const persistence::CatalogFields& columns);

// A header-only CSV with every field.
std::string template_csv();
// The Error rows as CSV: line, messages, then the row's fields.
std::string errors_csv(const SampleImportPlan& plan);

}  // namespace nobion::entry
