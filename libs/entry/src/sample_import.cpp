#include "pychron/entry/sample_import.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "pychron/entry/names.hpp"
#include "pychron/entry/sample_fields.hpp"
#include "pychron/core/number.hpp"

namespace pychron::entry {

namespace ps = persistence;

namespace {

struct FieldInfo {
  ImportField field;
  const char* name;
  std::vector<const char*> aliases;
};

const std::vector<FieldInfo>& infos() {
  static const std::vector<FieldInfo> all = {
      {ImportField::Sample, "sample", {"sample_name", "name"}},
      {ImportField::Project, "project", {"project_name"}},
      {ImportField::PrincipalInvestigator, "principal_investigator", {"pi", "principal investigator", "investigator"}},
      {ImportField::PrincipalInvestigatorKind, "principal_investigator_kind", {"pi_kind", "pi kind"}},
      {ImportField::PrincipalInvestigatorOrcid, "principal_investigator_orcid", {"pi_orcid", "pi orcid", "orcid"}},
      {ImportField::PrincipalInvestigatorUuid, "principal_investigator_uuid", {"pi_uuid", "pi uuid"}},
      {ImportField::Material, "material", {"material_name", "mineral"}},
      {ImportField::Grainsize, "grainsize", {"grain_size", "grain size", "size"}},
      {ImportField::Note, "note", {"notes", "comment", "comments", "description"}},
      {ImportField::Igsn, "igsn", {}},
      {ImportField::Lat, "lat", {"latitude"}},
      {ImportField::Lon, "lon", {"long", "longitude", "lng"}},
      {ImportField::Elevation, "elevation", {"elev", "altitude"}},
      {ImportField::StorageLocation, "storage_location", {"storage", "storage location"}},
      {ImportField::Location, "location", {"locality", "site"}},
      {ImportField::Unit, "unit", {"geologic_unit", "formation"}},
      {ImportField::Lithology, "lithology", {"rock_type"}},
      {ImportField::LithologyClass, "lithology_class", {}},
      {ImportField::LithologyType, "lithology_type", {}},
      {ImportField::LithologyGroup, "lithology_group", {}},
      {ImportField::ApproximateAge, "approximate_age", {"age", "approx_age", "estimated_age"}},
      {ImportField::Easting, "easting", {"utm_easting", "utm_e"}},
      {ImportField::Northing, "northing", {"utm_northing", "utm_n"}},
      {ImportField::Zone, "zone", {"utm_zone"}},
  };
  return all;
}

std::string normalize_header(std::string_view h) {
  std::string out;
  for (char c : trim(h)) {
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    out.push_back(l == '-' ? '_' : l);
  }
  return out;
}

std::optional<double> parse_number(const std::string& text, bool* bad) {
  const std::string t = trim(text);
  if (t.empty()) return std::nullopt;
  const auto v = parse_double(t);
  if (!v) {
    *bad = true;
    return std::nullopt;
  }
  return v;
}

std::optional<std::string> opt(const std::string& text) {
  std::string t = trim(text);
  if (t.empty()) return std::nullopt;
  return t;
}

std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

const ps::PrincipalInvestigatorRow* pi_by_uuid(const CatalogSnapshot& catalog, const ps::Uuid& uuid) {
  for (const auto& p : catalog.principal_investigators)
    if (p.uuid == uuid) return &p;
  return nullptr;
}

const ps::PrincipalInvestigatorRow* pi_by_orcid(const CatalogSnapshot& catalog, const std::string& orcid) {
  for (const auto& p : catalog.principal_investigators)
    if (p.orcid == orcid) return &p;
  return nullptr;
}

// How a PI named by name alone stands against the catalog: the one stored PI
// by exactly that name, or the namesakes to ask about, or neither (a new PI).
struct NameMatch {
  const ps::PrincipalInvestigatorRow* exact = nullptr;
  std::vector<ps::PrincipalInvestigatorRow> candidates;
};

// `kind_given`: the sheet said the kind; otherwise a name with no given name
// is also an organisation's of that name. A person's namesakes: persons of
// the same family name (case aside) whose given name has the same initial;
// every person of that family name when the sheet gives no given name.
NameMatch match_name(const CatalogSnapshot& catalog, const PiName& pi, bool kind_given) {
  NameMatch out;
  std::vector<const ps::PrincipalInvestigatorRow*> exact;
  const std::string family = ascii_lower(pi.family_name);
  const std::string initial = initial_of(pi.given_name);
  for (const auto& p : catalog.principal_investigators) {
    const bool same_kind = p.kind == pi.kind;
    const bool either_kind = !kind_given && pi.given_name.empty();
    if ((same_kind || either_kind) && p.family_name == pi.family_name && p.given_name == pi.given_name)
      exact.push_back(&p);
    if (!same_kind && !either_kind) continue;
    if (ascii_lower(p.family_name) != family) continue;
    if (p.kind == ps::PiKind::Person && !initial.empty() && initial_of(p.given_name) != initial) continue;
    out.candidates.push_back(p);
  }
  if (exact.size() == 1) {
    out.exact = exact.front();
    out.candidates.clear();
  }
  return out;
}

// A new PI of the sheet: by its ORCID when it has one (an ORCID is one
// person, and unique in the store), else by kind and name.
std::string pi_key(const PiName& pi, const std::optional<std::string>& orcid) {
  if (orcid) return "orcid\n" + *orcid;
  return std::string(ps::pi_kind_name(pi.kind)) + '\n' + pi.family_name + '\n' + pi.given_name;
}

// The PI a settled row names: the stored one, or none (a new one).
struct EffectivePi {
  bool waiting = false;  // asked and not answered
  std::optional<ps::Uuid> stored;
};

EffectivePi effective_pi(const ImportRow& row, const SampleImportPlan& plan) {
  if (!row.pi_asked) return {false, row.pi_uuid};
  const std::string name = question_name(row.pi);
  for (const auto& q : plan.questions)
    if (q.name == name) return q.answer ? EffectivePi{false, q.answer->pi} : EffectivePi{true, std::nullopt};
  return {true, std::nullopt};
}

// Resolves every row that is not in error against the catalog, given what the
// plan knows of its PI, and counts the rows and what they would create.
void settle(SampleImportPlan& plan, const CatalogSnapshot& catalog) {
  plan.creates = plan.exists = plan.updates = plan.asks = plan.errors = 0;
  std::set<std::string> new_pis, new_projects, new_materials;
  // (sample, project, PI, material, grainsize) -> line of first occurrence; the
  // PI as settled (a stored uuid, a new PI's key, or the question unanswered)
  std::map<std::tuple<std::string, std::string, std::string, std::string, std::string>, int> seen;
  for (auto& row : plan.rows) {
    if (!row.well_formed) {
      ++plan.errors;
      continue;
    }
    row.messages.clear();
    row.changed.clear();
    row.existing.reset();
    const EffectivePi pi = effective_pi(row, plan);
    const std::string pi_id = pi.waiting  ? "?" + question_name(row.pi)
                              : pi.stored ? pi.stored->str()
                                          : "+" + pi_key(row.pi, row.pi_orcid);
    const auto key = std::make_tuple(row.sample, row.project, pi_id, row.material, row.grainsize);
    if (auto [it, fresh] = seen.emplace(key, row.line); !fresh) {
      row.state = RowState::Error;
      row.messages.push_back("the same sample as line " + std::to_string(it->second));
      ++plan.errors;
      continue;
    }
    if (pi.waiting) {
      row.state = RowState::Ask;
      ++plan.asks;
      row.messages.push_back("principal investigator '" + row.principal_investigator +
                             "' has namesakes: say which (principal_investigator_orcid or principal_investigator_uuid)");
      continue;
    }
    const ps::ProjectRow* project_row = nullptr;
    if (pi.stored)
      for (const auto& p : catalog.projects)
        if (p.name == row.project && p.principal_investigator == *pi.stored) project_row = &p;
    const ps::MaterialRow* material_row = nullptr;
    for (const auto& m : catalog.materials)
      if (m.name == row.material && m.grainsize == row.grainsize) material_row = &m;
    const ps::SampleRow* sample_row = nullptr;
    if (project_row && material_row)
      for (const auto& s : catalog.samples)
        if (s.name == row.sample && s.project == project_row->uuid && s.material == material_row->uuid) sample_row = &s;

    if (!pi.stored)
      new_pis.insert(row.pi_orcid ? row.principal_investigator + " (" + *row.pi_orcid + ")" : row.principal_investigator);
    if (!project_row) new_projects.insert(row.project + " (" + row.principal_investigator + ")");
    if (!material_row)
      new_materials.insert(row.grainsize.empty() ? row.material : row.material + " (" + row.grainsize + ")");

    if (!sample_row) {
      row.state = RowState::Create;
      ++plan.creates;
      continue;
    }
    row.existing = sample_row->uuid;
    const ps::CatalogFields given = sample_columns(row.fields);
    const ps::CatalogFields stored = sample_columns(sample_row->fields);
    for (const auto& [name, value] : given)
      if (!std::holds_alternative<std::monostate>(value) && stored.at(name) != value) row.changed.push_back(name);
    if (row.changed.empty()) {
      row.state = RowState::Exists;
      ++plan.exists;
    } else {
      row.state = RowState::Update;
      ++plan.updates;
      if (!plan.update_existing) row.messages.emplace_back("differs from the stored sample; not updated");
    }
  }
  plan.new_principal_investigators.assign(new_pis.begin(), new_pis.end());
  plan.new_projects.assign(new_projects.begin(), new_projects.end());
  plan.new_materials.assign(new_materials.begin(), new_materials.end());
}

// Reads the four PI cells of a row into row.pi, pi_orcid, pi_uuid and
// pi_asked, or adds to its messages (PI spec 6). The uuid is the PI itself;
// an ORCID is matched next and is that PI whatever the name cell says, but a
// family name that differs from that PI's is an error; a name alone is the
// one stored PI by exactly that name, or a question when it has namesakes:
// those are returned.
std::vector<ps::PrincipalInvestigatorRow> resolve_pi(ImportRow& row, const std::string& name,
                                                     const std::string& kind_text, const std::string& orcid_text,
                                                     const std::string& uuid_text, const CatalogSnapshot& catalog) {
  row.principal_investigator = name;
  std::string kind_name = ascii_lower(kind_text);
  if (kind_name == "organization") kind_name = "organisation";
  const auto kind = kind_name.empty() ? std::optional<ps::PiKind>(ps::PiKind::Person) : ps::parse_pi_kind(kind_name);
  if (!kind) {
    row.messages.push_back("principal_investigator_kind '" + kind_text + "': write person or organisation");
    return {};
  }
  if (!orcid_text.empty()) {
    auto orcid = parse_orcid(orcid_text);
    if (!orcid) {
      row.messages.push_back(orcid.error().what);
      return {};
    }
    row.pi_orcid = *orcid;
  }
  const ps::PrincipalInvestigatorRow* stored = nullptr;
  if (!uuid_text.empty()) {
    const auto uuid = ps::Uuid::parse(uuid_text);
    stored = uuid ? pi_by_uuid(catalog, *uuid) : nullptr;
    if (!stored) {
      row.messages.push_back("no principal investigator with uuid '" + uuid_text + "'");
      return {};
    }
  } else if (row.pi_orcid) {
    stored = pi_by_orcid(catalog, *row.pi_orcid);
  }
  if (name.empty() && !stored) {
    row.messages.emplace_back("no principal investigator");
    return {};
  }
  if (!name.empty()) {
    auto parsed = *kind == ps::PiKind::Organisation ? parse_organisation(name) : parse_pi(name);
    if (!parsed) {
      row.messages.push_back(parsed.error().what);
      return {};
    }
    row.pi = *parsed;
    if (auto ok = check_pi(row.pi, row.pi_orcid); !ok) {
      row.messages.push_back(ok.error().what);
      return {};
    }
    if (stored && uuid_text.empty() && ascii_lower(stored->family_name) != ascii_lower(row.pi.family_name)) {
      row.messages.push_back("ORCID " + *row.pi_orcid + " is " + stored->display_name + "'s, not " +
                             row.pi.family_name + "'s");
      return {};
    }
  }
  if (stored) {
    row.pi_uuid = stored->uuid;
    row.pi = PiName{stored->family_name, stored->given_name, stored->kind};
    row.principal_investigator = sheet_name(row.pi);
    return {};
  }
  row.principal_investigator = sheet_name(row.pi);
  if (row.pi_orcid) return {};  // a new person with this ORCID
  NameMatch match = match_name(catalog, row.pi, !kind_text.empty());
  if (match.exact) {
    row.pi_uuid = match.exact->uuid;
    row.pi = PiName{match.exact->family_name, match.exact->given_name, match.exact->kind};
    return {};
  }
  row.pi_asked = !match.candidates.empty();
  return std::move(match.candidates);
}
}  // namespace

std::string question_name(const PiName& pi) {
  return pi.kind == ps::PiKind::Organisation ? pi.family_name + " (organisation)" : sheet_name(pi);
}

bool SampleImportPlan::ready() const {
  return std::all_of(questions.begin(), questions.end(), [](const PiQuestion& q) { return q.answer.has_value(); });
}

const std::vector<ImportField>& import_fields() {
  static const std::vector<ImportField> all = [] {
    std::vector<ImportField> out;
    for (const auto& i : infos()) out.push_back(i.field);
    return out;
  }();
  return all;
}

std::string_view field_name(ImportField field) {
  for (const auto& i : infos())
    if (i.field == field) return i.name;
  return "";
}

std::optional<ImportField> field_for_header(std::string_view header) {
  const std::string h = normalize_header(header);
  for (const auto& i : infos()) {
    if (h == i.name) return i.field;
    for (const char* a : i.aliases)
      if (h == normalize_header(a)) return i.field;
  }
  return std::nullopt;
}

ColumnMapping default_mapping(const std::vector<std::string>& header) {
  ColumnMapping out;
  std::set<ImportField> used;
  for (const auto& h : header) {
    auto f = field_for_header(h);
    if (f && !used.insert(*f).second) f.reset();  // the first column of a field wins
    out.push_back(f);
  }
  return out;
}

Result<CatalogSnapshot> read_snapshot(ps::IStore& store) {
  CatalogSnapshot s;
  auto pis = store.principal_investigators();
  if (!pis) return fail(pis.error());
  s.principal_investigators = std::move(*pis);
  auto projects = store.projects(std::nullopt);
  if (!projects) return fail(projects.error());
  s.projects = std::move(*projects);
  auto materials = store.materials();
  if (!materials) return fail(materials.error());
  s.materials = std::move(*materials);
  ps::SampleQuery all;
  all.limit = 100'000'000;
  auto samples = store.samples(all);
  if (!samples) return fail(samples.error());
  s.samples = std::move(*samples);
  return s;
}

std::string_view to_string(RowState state) {
  switch (state) {
    case RowState::Create: return "create";
    case RowState::Exists: return "exists";
    case RowState::Update: return "update";
    case RowState::Error: return "error";
    case RowState::Ask: return "ask";
  }
  return "";
}

ps::CatalogFields sample_columns(const ps::SampleFields& f) {
  const auto text = [](const std::optional<std::string>& v) -> ps::CatalogValue {
    return v ? ps::CatalogValue{*v} : ps::CatalogValue{};
  };
  const auto num = [](const std::optional<double>& v) -> ps::CatalogValue {
    return v ? ps::CatalogValue{*v} : ps::CatalogValue{};
  };
  return {{"note", text(f.note)},
          {"igsn", text(f.igsn)},
          {"lat", num(f.lat)},
          {"lon", num(f.lon)},
          {"elevation", num(f.elevation)},
          {"storage_location", text(f.storage_location)},
          {"location", text(f.location)},
          {"unit", text(f.unit)},
          {"lithology", text(f.lithology)},
          {"lithology_class", text(f.lithology_class)},
          {"lithology_type", text(f.lithology_type)},
          {"lithology_group", text(f.lithology_group)},
          {"approximate_age", num(f.approximate_age)}};
}

ps::SampleFields sample_fields_of(const ps::CatalogFields& columns) {
  ps::SampleFields f;
  const auto text = [&](const char* name, std::optional<std::string>& out) {
    const auto it = columns.find(name);
    if (it != columns.end())
      if (const auto* s = std::get_if<std::string>(&it->second)) out = *s;
  };
  const auto num = [&](const char* name, std::optional<double>& out) {
    const auto it = columns.find(name);
    if (it == columns.end()) return;
    if (const auto* d = std::get_if<double>(&it->second)) out = *d;
    if (const auto* i = std::get_if<std::int64_t>(&it->second)) out = static_cast<double>(*i);
  };
  text("note", f.note);
  text("igsn", f.igsn);
  num("lat", f.lat);
  num("lon", f.lon);
  num("elevation", f.elevation);
  text("storage_location", f.storage_location);
  text("location", f.location);
  text("unit", f.unit);
  text("lithology", f.lithology);
  text("lithology_class", f.lithology_class);
  text("lithology_type", f.lithology_type);
  text("lithology_group", f.lithology_group);
  num("approximate_age", f.approximate_age);
  return f;
}

SampleImportPlan plan_sample_import(const CsvTable& table, const ColumnMapping& mapping, const CatalogSnapshot& catalog,
                                    const ImportOptions& options) {
  SampleImportPlan plan;
  plan.update_existing = options.update_existing;
  std::map<std::string, std::pair<int, PiName>> new_orcids;  // ORCID -> line and name of its first row
  for (std::size_t r = 0; r < table.rows.size(); ++r) {
    const auto& cells = table.rows[r];
    ImportRow row;
    row.line = table.lines[r];
    std::map<ImportField, std::string> v;
    for (std::size_t c = 0; c < mapping.size() && c < cells.size(); ++c)
      if (mapping[c]) v[*mapping[c]] = cells[c];
    const auto get = [&](ImportField f) { return v.contains(f) ? trim(v[f]) : std::string(); };
    if (std::find(table.ragged.begin(), table.ragged.end(), row.line) != table.ragged.end())
      row.messages.emplace_back("the row does not have one value per column");

    row.sample = get(ImportField::Sample);
    row.project = get(ImportField::Project);
    row.material = get(ImportField::Material);
    row.grainsize = get(ImportField::Grainsize);
    if (row.sample.empty()) row.messages.emplace_back("no sample name");
    if (row.project.empty()) row.messages.emplace_back("no project");
    if (row.material.empty()) row.messages.emplace_back("no material");
    auto namesakes =
        resolve_pi(row, get(ImportField::PrincipalInvestigator), get(ImportField::PrincipalInvestigatorKind),
                   get(ImportField::PrincipalInvestigatorOrcid), get(ImportField::PrincipalInvestigatorUuid), catalog);
    // An ORCID not stored is a new person, one for the whole sheet: named as
    // its first row names it, and refused under another family name.
    if (row.pi_orcid && !row.pi_uuid && !row.pi.family_name.empty()) {
      const auto [first, fresh] = new_orcids.try_emplace(*row.pi_orcid, row.line, row.pi);
      if (!fresh && ascii_lower(first->second.second.family_name) != ascii_lower(row.pi.family_name)) {
        row.messages.push_back("ORCID " + *row.pi_orcid + " is " + sheet_name(first->second.second) + "'s on line " +
                               std::to_string(first->second.first) + ", not " + row.pi.family_name + "'s");
      } else if (!fresh) {
        row.pi = first->second.second;
        row.principal_investigator = sheet_name(row.pi);
      }
    }
    if (!row.project.empty() && !valid_project_name(row.project))
      row.messages.push_back("project '" + row.project + "': start with a letter; use letters, digits, '-' or '_'");

    auto& f = row.fields;
    f.note = opt(get(ImportField::Note));
    f.igsn = opt(get(ImportField::Igsn));
    f.storage_location = opt(get(ImportField::StorageLocation));
    f.location = opt(get(ImportField::Location));
    f.unit = opt(get(ImportField::Unit));
    f.lithology = opt(get(ImportField::Lithology));
    f.lithology_class = opt(get(ImportField::LithologyClass));
    f.lithology_type = opt(get(ImportField::LithologyType));
    f.lithology_group = opt(get(ImportField::LithologyGroup));
    const auto number = [&](ImportField field) {
      bool bad = false;
      auto n = parse_number(get(field), &bad);
      if (bad) row.messages.push_back(std::string(field_name(field)) + " '" + get(field) + "' is not a number");
      return n;
    };
    f.lat = number(ImportField::Lat);
    f.lon = number(ImportField::Lon);
    f.elevation = number(ImportField::Elevation);
    f.approximate_age = number(ImportField::ApproximateAge);
    const auto easting = number(ImportField::Easting);
    const auto northing = number(ImportField::Northing);
    const std::string zone = get(ImportField::Zone);
    if (!f.lat && !f.lon && (easting || northing || !zone.empty())) {
      if (!easting || !northing || zone.empty()) {
        row.messages.emplace_back("UTM needs easting, northing and zone");
      } else {
        auto ll = utm_to_lat_lon(*easting, *northing, zone);
        if (ll) {
          f.lat = ll->lat;
          f.lon = ll->lon;
        } else {
          row.messages.push_back(ll.error().what);
        }
      }
    }
    if (auto ok = check_lat_lon(f.lat, f.lon); !ok) row.messages.push_back(ok.error().what);

    row.well_formed = row.messages.empty();
    row.state = row.well_formed ? RowState::Create : RowState::Error;  // settle() says which
    if (row.well_formed && row.pi_asked) {
      const std::string name = question_name(row.pi);
      auto q = std::find_if(plan.questions.begin(), plan.questions.end(),
                            [&](const PiQuestion& x) { return x.name == name; });
      if (q == plan.questions.end()) {
        q = plan.questions.insert(plan.questions.end(), PiQuestion{name, row.pi, {}, {}, std::nullopt});
      }
      // Rows of one name may differ in candidates (one with no kind column
      // also matches an organisation of that name): the question has them all.
      for (auto& c : namesakes)
        if (std::none_of(q->candidates.begin(), q->candidates.end(),
                         [&](const ps::PrincipalInvestigatorRow& x) { return x.uuid == c.uuid; }))
          q->candidates.push_back(std::move(c));
      q->lines.push_back(row.line);
    }
    plan.rows.push_back(std::move(row));
  }
  settle(plan, catalog);
  return plan;
}

Result<void> SampleImportPlan::answer(std::string_view name, PiAnswer given, const CatalogSnapshot& catalog) {
  const auto q =
      std::find_if(questions.begin(), questions.end(), [&](const PiQuestion& x) { return x.name == name; });
  if (q == questions.end())
    return fail(ErrorKind::Config, "no question about principal investigator '" + std::string(name) + "'");
  if (given.pi && std::none_of(q->candidates.begin(), q->candidates.end(),
                               [&](const ps::PrincipalInvestigatorRow& c) { return c.uuid == *given.pi; }))
    return fail(ErrorKind::Config, "principal investigator " + given.pi->str() + " is not one of the namesakes of '" +
                                       std::string(name) + "'");
  q->answer = given;
  settle(*this, catalog);
  return {};
}

Result<ps::CatalogEditBatch> to_batch(const SampleImportPlan& plan, const CatalogSnapshot& catalog,
                                      const ImportOptions& options) {
  if (!plan.ready()) {
    std::string names;
    for (const auto& q : plan.questions)
      if (!q.answer) names += (names.empty() ? "'" : ", '") + q.name + "'";
    return fail(ErrorKind::Config, "say which principal investigator is meant by " + names + " first");
  }
  ps::CatalogEditBatch batch;
  batch.message = "sample import";
  std::vector<ps::CatalogEdit> pis, projects, materials, samples;
  std::map<std::string, ps::Uuid> new_pi_ids;  // pi_key -> the PI this batch makes
  std::map<std::pair<std::string, ps::Uuid>, ps::Uuid> project_ids;
  std::map<std::pair<std::string, std::string>, ps::Uuid> material_ids;
  for (const auto& p : catalog.projects)
    if (p.principal_investigator) project_ids[{p.name, *p.principal_investigator}] = p.uuid;
  for (const auto& m : catalog.materials) material_ids[{m.name, m.grainsize}] = m.uuid;

  for (const auto& row : plan.rows) {
    if (row.state == RowState::Update && options.update_existing && row.existing) {
      const ps::SampleRow* stored = nullptr;
      for (const auto& s : catalog.samples)
        if (s.uuid == *row.existing) stored = &s;
      if (!stored) continue;
      const ps::CatalogFields given = sample_columns(row.fields);
      const ps::CatalogFields before = sample_columns(stored->fields);
      ps::CatalogUpdate u{ps::CatalogTable::Sample, *row.existing, {}, {}};
      for (const auto& name : row.changed) {
        u.expected[name] = before.at(name);
        u.values[name] = given.at(name);
      }
      samples.emplace_back(std::move(u));
      continue;
    }
    if (row.state != RowState::Create) continue;
    std::optional<ps::Uuid> pi = effective_pi(row, plan).stored;
    if (!pi) {
      const std::string key = pi_key(row.pi, row.pi_orcid);
      auto it = new_pi_ids.find(key);
      if (it == new_pi_ids.end()) {
        const ps::Uuid id = ps::Uuid::v7();
        it = new_pi_ids.emplace(key, id).first;
        ps::CatalogFields values{{"kind", std::string(ps::pi_kind_name(row.pi.kind))},
                                 {"family_name", row.pi.family_name},
                                 {"given_name", row.pi.given_name}};
        if (row.pi_orcid) values["orcid"] = *row.pi_orcid;
        pis.emplace_back(ps::CatalogInsert{ps::CatalogTable::PrincipalInvestigator, id, std::move(values)});
      }
      pi = it->second;
    }
    auto project_it = project_ids.find({row.project, *pi});
    if (project_it == project_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      project_it = project_ids.emplace(std::make_pair(row.project, *pi), id).first;
      projects.emplace_back(ps::CatalogInsert{ps::CatalogTable::Project, id, {{"name", row.project}, {"pi_uuid", *pi}}});
    }
    auto material_it = material_ids.find({row.material, row.grainsize});
    if (material_it == material_ids.end()) {
      const ps::Uuid id = ps::Uuid::v7();
      material_it = material_ids.emplace(std::make_pair(row.material, row.grainsize), id).first;
      materials.emplace_back(ps::CatalogInsert{ps::CatalogTable::Material, id, {{"name", row.material}, {"grainsize", row.grainsize}}});
    }
    ps::CatalogFields values = sample_columns(row.fields);
    for (auto it = values.begin(); it != values.end();)
      it = std::holds_alternative<std::monostate>(it->second) ? values.erase(it) : std::next(it);
    values["name"] = row.sample;
    values["project_uuid"] = project_it->second;
    values["material_uuid"] = material_it->second;
    samples.emplace_back(ps::CatalogInsert{ps::CatalogTable::Sample, ps::Uuid::v7(), std::move(values)});
  }
  for (auto* part : {&pis, &projects, &materials, &samples})
    for (auto& e : *part) batch.edits.push_back(std::move(e));
  return batch;
}

std::string template_csv() {
  std::vector<std::string> header;
  for (const auto f : import_fields()) header.emplace_back(field_name(f));
  return write_csv(header, {});
}

std::string errors_csv(const SampleImportPlan& plan) {
  std::vector<std::string> header{"line", "messages", "sample", "project", "principal_investigator", "material",
                                  "grainsize"};
  std::vector<std::vector<std::string>> rows;
  for (const auto& r : plan.rows) {
    if (r.state != RowState::Error) continue;
    std::string messages;
    for (const auto& m : r.messages) messages += (messages.empty() ? "" : "; ") + m;
    rows.push_back({std::to_string(r.line), messages, r.sample, r.project, r.principal_investigator, r.material,
                    r.grainsize});
  }
  return write_csv(header, rows);
}

}  // namespace pychron::entry
