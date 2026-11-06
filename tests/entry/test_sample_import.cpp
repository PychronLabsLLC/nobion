#include <gtest/gtest.h>

#include <algorithm>

#include "catalog_fixture.hpp"
#include "nobion/entry/sample_import.hpp"
#include "nobion/entry/sample_search.hpp"

using namespace pychron;
using namespace pychron::entry;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;

namespace {

CatalogSnapshot snapshot() {
  CatalogSnapshot c;
  const Uuid ross = Uuid::v7(), alpha = Uuid::v7(), san = Uuid::v7(), bio = Uuid::v7();
  PrincipalInvestigatorRow pi;
  pi.uuid = ross;
  pi.family_name = "Ross";
  pi.given_name = "J";
  pi.display_name = "J Ross";
  c.principal_investigators = {pi};
  c.projects = {{alpha, "Alpha", ross, "J Ross", std::nullopt, std::nullopt, std::nullopt, std::nullopt, 1}};
  c.materials = {{san, "sanidine", "", 1}, {bio, "biotite", "20-40", 0}};
  SampleRow fc2;
  fc2.uuid = Uuid::v7();
  fc2.name = "FC-2";
  fc2.project = alpha;
  fc2.material = san;
  fc2.principal_investigator = ross;
  fc2.project_name = "Alpha";
  fc2.principal_investigator_name = "J Ross";
  fc2.material_name = "sanidine";
  fc2.fields.lat = 34.0;
  fc2.fields.lon = -106.0;
  c.samples = {fc2};
  return c;
}

SampleImportPlan plan(const std::string& csv, const CatalogSnapshot& c, ImportOptions o = {}) {
  auto t = read_csv(csv);
  EXPECT_TRUE(t);
  return plan_sample_import(*t, default_mapping(t->header), c, o);
}

}  // namespace

TEST(SampleImport, HeaderAliases) {
  const auto m = default_mapping({"Sample", "PI", "Latitude", "grain size", "mystery", "sample"});
  EXPECT_EQ(m[0], ImportField::Sample);
  EXPECT_EQ(m[1], ImportField::PrincipalInvestigator);
  EXPECT_EQ(m[2], ImportField::Lat);
  EXPECT_EQ(m[3], ImportField::Grainsize);
  EXPECT_FALSE(m[4]);
  EXPECT_FALSE(m[5]);  // the first column of a field wins
}

TEST(SampleImport, EveryRowState) {
  const auto c = snapshot();
  const auto p = plan(
      "sample,project,pi,material,grainsize,lat,lon\n"
      "FC-2,Alpha,\"Ross, J\",sanidine,,34,-106\n"      // exists
      "FC-2,Alpha,\"Ross, J\",sanidine,,35,-106\n"      // the same sample again: error
      "new-1,Alpha,\"Ross, J\",biotite,20-40,,\n"      // create
      "new-2,Beta,Smith,quartz,,,\n",                   // create with a new PI, project, material
      c);
  ASSERT_EQ(p.rows.size(), 4u);
  EXPECT_EQ(p.rows[0].state, RowState::Exists);
  EXPECT_EQ(p.rows[1].state, RowState::Error);
  EXPECT_NE(p.rows[1].messages.front().find("line 2"), std::string::npos);
  EXPECT_EQ(p.rows[2].state, RowState::Create);
  EXPECT_EQ(p.rows[3].state, RowState::Create);
  EXPECT_EQ(p.creates, 2);
  EXPECT_EQ(p.exists, 1);
  EXPECT_EQ(p.errors, 1);
  EXPECT_EQ(p.new_principal_investigators, (std::vector<std::string>{"Smith"}));
  EXPECT_EQ(p.new_projects, (std::vector<std::string>{"Beta (Smith)"}));
  EXPECT_EQ(p.new_materials, (std::vector<std::string>{"quartz"}));
}

TEST(SampleImport, UpdateOnlyWhenAsked) {
  const auto c = snapshot();
  const std::string csv = "sample,project,pi,material,lat,lon,note\nFC-2,Alpha,\"Ross, J\",sanidine,34.5,-106,hi\n";
  auto p = plan(csv, c);
  ASSERT_EQ(p.rows[0].state, RowState::Update);
  EXPECT_EQ(p.rows[0].changed, (std::vector<std::string>{"lat", "note"}));
  EXPECT_TRUE(to_batch(p, c, {})->edits.empty());
  ImportOptions o;
  o.update_existing = true;
  p = plan(csv, c, o);
  const auto batch = to_batch(p, c, o);
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->edits.size(), 1u);
  const auto& u = std::get<CatalogUpdate>(batch->edits[0]);
  EXPECT_EQ(u.expected.at("lat"), CatalogValue{34.0});
  EXPECT_EQ(u.expected.at("note"), CatalogValue{});
  EXPECT_EQ(u.values.at("lat"), CatalogValue{34.5});
}

TEST(SampleImport, EveryErrorOfARowIsListed) {
  const auto c = snapshot();
  const auto p = plan("sample,project,pi,material,lat,lon,elevation\n,1bad,jake ross,,95,,high\n", c);
  ASSERT_EQ(p.rows.size(), 1u);
  const auto& m = p.rows[0].messages;
  const auto has = [&](const char* text) {
    return std::any_of(m.begin(), m.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
  };
  EXPECT_TRUE(has("no sample name"));
  EXPECT_TRUE(has("no material"));
  EXPECT_TRUE(has("Family, Given"));
  EXPECT_TRUE(has("project '1bad'"));
  EXPECT_TRUE(has("elevation 'high'"));
  EXPECT_TRUE(has("go together"));
}

TEST(SampleImport, GrainsizeIsPartOfTheMaterial) {
  const auto c = snapshot();
  // biotite exists only with grainsize 20-40: plain biotite is a new material.
  const auto p = plan("sample,project,pi,material\nb-1,Alpha,\"Ross, J\",biotite\n", c);
  EXPECT_EQ(p.new_materials, (std::vector<std::string>{"biotite"}));
}

TEST(SampleImport, UtmFillsLatLon) {
  const auto c = snapshot();
  auto p = plan("sample,project,pi,material,easting,northing,zone\nu-1,Alpha,\"Ross, J\",sanidine,323394,4307396,13S\n", c);
  ASSERT_EQ(p.rows[0].state, RowState::Create);
  ASSERT_TRUE(p.rows[0].fields.lat);
  EXPECT_NEAR(*p.rows[0].fields.lat, 38.8977, 1e-3);
  p = plan("sample,project,pi,material,lat,lon,easting,northing,zone\nu-2,Alpha,\"Ross, J\",sanidine,1,2,323394,4307396,13S\n", c);
  EXPECT_EQ(*p.rows[0].fields.lat, 1.0);  // lat/lon given: UTM ignored
  p = plan("sample,project,pi,material,easting\nu-3,Alpha,\"Ross, J\",sanidine,323394\n", c);
  EXPECT_EQ(p.rows[0].state, RowState::Error);
}

TEST(SampleImport, TemplateParsesBack) {
  auto t = read_csv(template_csv());
  ASSERT_TRUE(t);
  const auto m = default_mapping(t->header);
  ASSERT_EQ(m.size(), import_fields().size());
  for (std::size_t i = 0; i < m.size(); ++i) EXPECT_EQ(m[i], import_fields()[i]);
}

TEST(SampleImport, BatchInsertsInKeyOrderOnce) {
  const auto c = snapshot();
  const auto p = plan("sample,project,pi,material\na,Beta,Smith,quartz\nb,Beta,Smith,quartz\n", c);
  const auto batch = to_batch(p, c, {});
  ASSERT_TRUE(batch);
  ASSERT_EQ(batch->edits.size(), 5u);  // one PI, one project, one material, two samples
  EXPECT_EQ(std::get<CatalogInsert>(batch->edits[0]).table, CatalogTable::PrincipalInvestigator);
  EXPECT_EQ(std::get<CatalogInsert>(batch->edits[1]).table, CatalogTable::Project);
  EXPECT_EQ(std::get<CatalogInsert>(batch->edits[2]).table, CatalogTable::Material);
  EXPECT_EQ(std::get<CatalogInsert>(batch->edits[3]).table, CatalogTable::Sample);
}

namespace {

constexpr const char* kOrcid = "0000-0002-1825-0097";

PrincipalInvestigatorRow person(const std::string& family, const std::string& given,
                                std::optional<std::string> orcid = std::nullopt) {
  PrincipalInvestigatorRow p;
  p.uuid = Uuid::v7();
  p.family_name = family;
  p.given_name = given;
  p.display_name = given.empty() ? family : given + " " + family;
  p.orcid = std::move(orcid);
  return p;
}

const CatalogInsert* pi_insert(const CatalogEditBatch& batch) {
  for (const auto& e : batch.edits)
    if (const auto* i = std::get_if<CatalogInsert>(&e); i && i->table == CatalogTable::PrincipalInvestigator) return i;
  return nullptr;
}

bool says(const ImportRow& row, const std::string& text) {
  return std::any_of(row.messages.begin(), row.messages.end(),
                     [&](const std::string& m) { return m.find(text) != std::string::npos; });
}

}  // namespace

TEST(SampleImport, ThePiColumnsOfASheet) {
  const auto m = default_mapping({"principal_investigator_kind", "PI ORCID", "pi_uuid", "orcid"});
  EXPECT_EQ(m[0], ImportField::PrincipalInvestigatorKind);
  EXPECT_EQ(m[1], ImportField::PrincipalInvestigatorOrcid);
  EXPECT_EQ(m[2], ImportField::PrincipalInvestigatorUuid);
  EXPECT_FALSE(m[3]);  // the first column of a field wins
  EXPECT_NE(template_csv().find("principal_investigator_orcid"), std::string::npos);
}

TEST(SampleImport, AnOrcidWinsOverTheNameCell) {
  auto c = snapshot();
  c.principal_investigators.front().orcid = kOrcid;
  const Uuid ross = c.principal_investigators.front().uuid;
  const auto p = plan(std::string("sample,project,pi,material,principal_investigator_orcid\n") +
                          "FC-2,Alpha,\"Ross, Jake\",sanidine,https://orcid.org/" + kOrcid + "\n"  // that PI
                          "n-1,Alpha,,sanidine," + kOrcid + "\n"                                    // no name: that PI
                          "n-2,Alpha,\"Smith, J\",sanidine," + kOrcid + "\n"                        // another family name
                          "n-3,Alpha,\"Ross, J\",sanidine,0000-0002-1825-0098\n",                   // a bad ORCID
                      c);
  ASSERT_EQ(p.rows.size(), 4u);
  EXPECT_EQ(p.rows[0].state, RowState::Exists);
  EXPECT_EQ(p.rows[0].pi_uuid, ross);
  EXPECT_EQ(p.rows[1].state, RowState::Create);
  EXPECT_EQ(p.rows[1].pi_uuid, ross);
  EXPECT_EQ(p.rows[1].principal_investigator, "Ross, J");
  EXPECT_EQ(p.rows[2].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[2], "J Ross")) << p.rows[2].messages.front();
  EXPECT_TRUE(says(p.rows[2], "Smith")) << p.rows[2].messages.front();
  EXPECT_EQ(p.rows[3].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[3], "'0000-0002-1825-0098'"));
  EXPECT_TRUE(p.questions.empty());
  EXPECT_TRUE(p.new_principal_investigators.empty());
}

TEST(SampleImport, AnUnknownOrcidIsANewPersonWithIt) {
  const auto c = snapshot();
  // a namesake of the stored J Ross, but the ORCID says who: nothing to ask
  const auto p = plan(std::string("sample,project,pi,material,pi_orcid\nn-1,Beta,\"Ross, Jake\",sanidine,") + kOrcid +
                          "\n",
                      c);
  ASSERT_EQ(p.rows[0].state, RowState::Create);
  EXPECT_TRUE(p.questions.empty());
  const auto batch = to_batch(p, c, {});
  ASSERT_TRUE(batch);
  const auto* pi = pi_insert(*batch);
  ASSERT_NE(pi, nullptr);
  EXPECT_EQ(pi->values.at("family_name"), CatalogValue{std::string("Ross")});
  EXPECT_EQ(pi->values.at("given_name"), CatalogValue{std::string("Jake")});
  EXPECT_EQ(pi->values.at("kind"), CatalogValue{std::string("person")});
  EXPECT_EQ(pi->values.at("orcid"), CatalogValue{std::string(kOrcid)});
}

// An ORCID is one person in the sheet too: rows giving it under two writings
// of one name make one PI (the first row's), and under another family name
// are an error naming both, as against a stored PI.
TEST(SampleImport, OneUnknownOrcidIsOneNewPersonInTheSheet) {
  const auto c = snapshot();
  const std::string orcid(kOrcid);
  const auto p = plan("sample,project,pi,material,pi_orcid\n"
                      "n-1,Beta,\"Ross, Jake\",sanidine," + orcid + "\n"
                      "n-2,Beta,\"Ross, J\",sanidine," + orcid + "\n"
                      "n-3,Beta,\"Smith, Jake\",sanidine," + orcid + "\n",
                      c);
  ASSERT_EQ(p.rows.size(), 3U);
  EXPECT_EQ(p.rows[0].state, RowState::Create);
  EXPECT_EQ(p.rows[1].state, RowState::Create);
  ASSERT_EQ(p.rows[2].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[2], "Ross, Jake")) << p.rows[2].messages.front();
  EXPECT_TRUE(says(p.rows[2], "Smith")) << p.rows[2].messages.front();
  EXPECT_TRUE(says(p.rows[2], "line 2")) << p.rows[2].messages.front();
  EXPECT_EQ(p.new_principal_investigators.size(), 1U);

  const auto ok = plan("sample,project,pi,material,pi_orcid\n"
                       "n-1,Beta,\"Ross, Jake\",sanidine," + orcid + "\n"
                       "n-2,Beta,\"Ross, J\",sanidine," + orcid + "\n",
                       c);
  const auto batch = to_batch(ok, c, {});
  ASSERT_TRUE(batch);
  const auto pis = std::count_if(batch->edits.begin(), batch->edits.end(), [](const auto& e) {
    const auto* i = std::get_if<CatalogInsert>(&e);
    return i && i->table == CatalogTable::PrincipalInvestigator;
  });
  EXPECT_EQ(pis, 1);
  EXPECT_EQ(pi_insert(*batch)->values.at("given_name"), CatalogValue{std::string("Jake")});
}

TEST(SampleImport, AUuidIsThePiItself) {
  auto c = snapshot();
  c.principal_investigators.push_back(person("Ross", "J"));  // a namesake: the uuid says which
  const Uuid ross = c.principal_investigators.front().uuid;
  const auto p = plan("sample,project,pi,material,principal_investigator_uuid\n"
                      "FC-2,Alpha,\"Ross, J\",sanidine," + ross.str() + "\n"
                      "n-1,Alpha,,sanidine," + ross.str() + "\n"
                      "n-2,Alpha,\"Ross, J\",sanidine," + Uuid::v7().str() + "\n"
                      "n-3,Alpha,\"Ross, J\",sanidine,not-a-uuid\n",
                      c);
  ASSERT_EQ(p.rows.size(), 4u);
  EXPECT_EQ(p.rows[0].state, RowState::Exists);
  EXPECT_EQ(p.rows[1].state, RowState::Create);
  EXPECT_EQ(p.rows[1].pi_uuid, ross);
  EXPECT_EQ(p.rows[2].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[2], "no principal investigator with uuid"));
  EXPECT_EQ(p.rows[3].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[3], "'not-a-uuid'"));
  EXPECT_TRUE(p.questions.empty());
}

// A person named by name alone is the one stored person of exactly that name;
// one whose family name and initial match others, and none of them exactly,
// is asked about: once a distinct name, and nothing is written until then.
TEST(SampleImport, ANamesakeIsAskedAboutOnceAName) {
  const auto c = snapshot();
  const Uuid ross = c.principal_investigators.front().uuid;
  auto p = plan("sample,project,pi,material\n"
                "n-1,Alpha,\"Ross, Jake\",sanidine\n"
                "n-2,Alpha,\"Ross, Jake\",sanidine\n"
                "n-3,Alpha,\"ross, Jill\",sanidine\n"  // a small letter is still that family name
                "n-4,Alpha,\"Ross, Kim\",sanidine\n"   // another initial: no namesake
                "n-5,Alpha,Ross,sanidine\n",           // no given name: every Ross is one
                c);
  ASSERT_EQ(p.rows.size(), 5u);
  EXPECT_EQ(p.rows[0].state, RowState::Ask);
  EXPECT_TRUE(says(p.rows[0], "principal_investigator_orcid"));
  EXPECT_EQ(p.rows[1].state, RowState::Ask);
  EXPECT_EQ(p.rows[2].state, RowState::Error);  // "ross" is all small letters
  EXPECT_EQ(p.rows[3].state, RowState::Create);
  EXPECT_EQ(p.rows[4].state, RowState::Ask);
  EXPECT_EQ(p.asks, 3);
  EXPECT_EQ(p.creates, 1);
  ASSERT_EQ(p.questions.size(), 2u);
  EXPECT_EQ(p.questions[0].name, "Ross, Jake");
  EXPECT_EQ(p.questions[0].lines, (std::vector<int>{2, 3}));
  ASSERT_EQ(p.questions[0].candidates.size(), 1u);
  EXPECT_EQ(p.questions[0].candidates[0].uuid, ross);
  EXPECT_EQ(p.questions[1].name, "Ross");
  EXPECT_FALSE(p.ready());
  const auto refused = to_batch(p, c, {});
  ASSERT_FALSE(refused);
  EXPECT_NE(refused.error().what.find("'Ross, Jake'"), std::string::npos) << refused.error().what;

  EXPECT_FALSE(p.answer("Ross, Jill", PiAnswer{ross}, c));          // nothing asked about it
  EXPECT_FALSE(p.answer("Ross, Jake", PiAnswer{Uuid::v7()}, c));  // not a candidate
  EXPECT_EQ(p.asks, 3);
  ASSERT_TRUE(p.answer("Ross, Jake", PiAnswer{ross}, c));
  EXPECT_EQ(p.rows[0].state, RowState::Create);
  EXPECT_EQ(p.rows[1].state, RowState::Create);
  EXPECT_FALSE(p.ready());
  ASSERT_TRUE(p.answer("Ross", PiAnswer{std::nullopt}, c));  // another person
  EXPECT_TRUE(p.ready());
  EXPECT_EQ(p.asks, 0);
  EXPECT_EQ(p.creates, 4);
  EXPECT_EQ(p.new_principal_investigators, (std::vector<std::string>{"Ross", "Ross, Kim"}));

  const auto batch = to_batch(p, c, {});
  ASSERT_TRUE(batch);
  int pis = 0;
  for (const auto& e : batch->edits)
    if (const auto* i = std::get_if<CatalogInsert>(&e); i && i->table == CatalogTable::PrincipalInvestigator) ++pis;
  EXPECT_EQ(pis, 2);  // "Ross, Kim" and the other Ross; Jake is the stored J Ross
}

TEST(SampleImport, TwoStoredPeopleOfOneNameAreAskedAbout) {
  auto c = snapshot();
  c.principal_investigators.push_back(person("Ross", "J"));
  const auto p = plan("sample,project,pi,material\nFC-2,Alpha,\"Ross, J\",sanidine\n", c);
  ASSERT_EQ(p.rows[0].state, RowState::Ask);
  ASSERT_EQ(p.questions.size(), 1u);
  EXPECT_EQ(p.questions[0].candidates.size(), 2u);
}

// Two rows of one sample are the same sample only when they name the same PI
// once it is settled: namesakes told apart by uuid are two samples; two names
// answered with one stored PI are one.
TEST(SampleImport, ADuplicateIsTheSamePiOnceSettled) {
  auto c = snapshot();
  const PrincipalInvestigatorRow other = person("Ross", "J");
  c.principal_investigators.push_back(other);
  const Uuid ross = c.principal_investigators.front().uuid;
  auto p = plan("sample,project,pi,material,pi_uuid\n"
                "d-1,Alpha,\"Ross, J\",sanidine," + ross.str() + "\n"
                "d-1,Alpha,\"Ross, J\",sanidine," + other.uuid.str() + "\n",
                c);
  EXPECT_EQ(p.errors, 0);
  EXPECT_EQ(p.creates, 2);

  const auto one = snapshot();
  const Uuid j = one.principal_investigators.front().uuid;
  p = plan("sample,project,pi,material\nd-2,Alpha,\"Ross, Jake\",sanidine\nd-2,Alpha,\"Ross, Jo\",sanidine\n", one);
  ASSERT_EQ(p.questions.size(), 2u);
  EXPECT_EQ(p.errors, 0);
  ASSERT_TRUE(p.answer("Ross, Jake", PiAnswer{j}, one));
  ASSERT_TRUE(p.answer("Ross, Jo", PiAnswer{j}, one));
  EXPECT_EQ(p.errors, 1);
  EXPECT_EQ(p.rows[1].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[1], "the same sample as line 2"));
  ASSERT_TRUE(p.answer("Ross, Jo", PiAnswer{std::nullopt}, one));  // changed: another person after all
  EXPECT_EQ(p.errors, 0);
  EXPECT_EQ(p.creates, 2);
}

TEST(SampleImport, AnOrganisationIsAKindOfPi) {
  auto c = snapshot();
  PrincipalInvestigatorRow lab = person("NMGRL", "");
  lab.kind = PiKind::Organisation;
  c.principal_investigators.push_back(lab);
  const auto p = plan(std::string("sample,project,pi,material,principal_investigator_kind,principal_investigator_orcid\n") +
                          "o-1,Gamma,\"Lab 7, NMT\",sanidine,organisation,\n"  // any name, as written
                          "o-2,Gamma,NMGRL,sanidine,,\n"                        // no kind: the organisation of that name
                          "o-3,Gamma,NMGRL,sanidine,organisation," + kOrcid + "\n"
                          "o-4,Gamma,NMGRL,sanidine,group,\n",
                      c);
  ASSERT_EQ(p.rows.size(), 4u);
  EXPECT_EQ(p.rows[0].state, RowState::Create);
  EXPECT_EQ(p.rows[0].pi.kind, PiKind::Organisation);
  EXPECT_EQ(p.rows[1].state, RowState::Create);
  EXPECT_EQ(p.rows[1].pi_uuid, lab.uuid);
  EXPECT_EQ(p.rows[2].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[2], "ORCID"));
  EXPECT_EQ(p.rows[3].state, RowState::Error);
  EXPECT_TRUE(says(p.rows[3], "'group'"));
  const auto batch = to_batch(p, c, {});
  ASSERT_TRUE(batch);
  const auto* pi = pi_insert(*batch);
  ASSERT_NE(pi, nullptr);
  EXPECT_EQ(pi->values.at("kind"), CatalogValue{std::string("organisation")});
  EXPECT_EQ(pi->values.at("family_name"), CatalogValue{std::string("Lab 7, NMT")});
  EXPECT_EQ(pi->values.at("given_name"), CatalogValue{std::string("")});
}

TEST(SampleSearch, NearDuplicates) {
  const auto c = snapshot();
  EXPECT_EQ(near_duplicates("fc 2", c.samples).size(), 1u);
  EXPECT_EQ(near_duplicates("FC_2", c.samples).size(), 1u);
  EXPECT_TRUE(near_duplicates("FC-3", c.samples).empty());
  EXPECT_TRUE(near_duplicates(" - ", c.samples).empty());
}

namespace {
class SampleImportStoreTest : public StoreTest {};
}  // namespace

TEST_P(SampleImportStoreTest, ImportThenReimportIsAllExists) {
  const std::string csv =
      "sample,project,principal_investigator,material,grainsize,lat,lon,note\n"
      "s-1,Delta,\"Doe, A\",sanidine,,34.1,-106.2,first\n"
      "s-2,Delta,\"Doe, A\",biotite,20-40,,,\n";
  auto t = read_csv(csv);
  ASSERT_TRUE(t);
  auto c = read_snapshot(*store_);
  ASSERT_TRUE(c);
  auto p = plan_sample_import(*t, default_mapping(t->header), *c, {});
  ASSERT_EQ(p.creates, 2);
  auto batch = to_batch(p, *c, {});
  ASSERT_TRUE(batch);
  auto out = store_->apply_catalog_edits(lab_.reduction_client, *batch);
  ASSERT_TRUE(out);
  ASSERT_TRUE(std::holds_alternative<CatalogApplied>(*out)) << describe(*out);

  c = read_snapshot(*store_);
  ASSERT_TRUE(c);
  p = plan_sample_import(*t, default_mapping(t->header), *c, {});
  EXPECT_EQ(p.exists, 2);
  EXPECT_EQ(p.creates, 0);
  EXPECT_TRUE(to_batch(p, *c, {})->edits.empty());
}

INSTANTIATE_TEST_SUITE_P(Engines, SampleImportStoreTest, ::testing::ValuesIn(engines()),
                         [](const auto& p) { return p.param; });
