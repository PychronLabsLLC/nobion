// Releases, embargoes and external identifiers (data publishing spec,
// sections 4.2 to 4.5), and loading an analysis as of a bookmark.

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pychron/persistence/publish.hpp"
#include "store_fixture.hpp"
#include "store_impl.hpp"

using namespace pychron;
using namespace pychron::persistence;
using namespace pychron::persistence::testing;
namespace pd = pychron::persistence::detail;

namespace {

UtcTime at(const char* iso) { return *UtcTime::parse(iso); }

// Two samples of one project and one of another, each with one analysis; a
// group and a bookmark over all three, as a release pins them.
class PublishStoreTest : public StoreTest {
 protected:
  void SetUp() override {
    StoreTest::SetUp();
    const Uuid c = lab_.reduction_client;
    project_ = *store_->add_project(c, {"Alpha"});
    project2_ = *store_->add_project(c, {"Beta"});
    material_ = *store_->add_material(c, {"sanidine"});
    sample1_ = *store_->add_sample(c, {"FC-1", project_, material_});
    sample2_ = *store_->add_sample(c, {"FC-2", project_, material_});
    sample3_ = *store_->add_sample(c, {"BT-1", project2_, material_});
    analysis1_ = ingest("70001", sample1_);
    analysis2_ = ingest("70002", sample2_);
    analysis3_ = ingest("70003", sample3_);
    group_ = *store_->create_group(reducer(), "release:r@v1", {analysis1_, analysis2_, analysis3_});
    bookmark_ = *store_->create_bookmark(reducer(), BookmarkSpec{"release r v1", std::nullopt, std::nullopt, group_});
  }

  Actor reducer() const { return Actor{lab_.reducer, lab_.reduction_client}; }

  Uuid ingest(const std::string& identifier, Uuid sample) {
    IdentifierSpec spec;
    spec.identifier = identifier;
    spec.sample = sample;
    EXPECT_TRUE(store_->add_identifier(lab_.acquisition_client, spec));
    const auto item = analysis_item(lab_, 1, series(1), series(0), identifier);
    auto ack = store_->ingest(item);
    EXPECT_TRUE(ack) << (ack ? "" : to_string(ack.error()));
    return std::get<AnalysisIngest>(item.body).analysis;
  }

  ReleaseInsert insert(const std::string& name, int version = 1) const {
    ReleaseInsert r;
    r.uuid = Uuid::v7();
    r.name = name;
    r.title = "Release " + name;
    r.version = version;
    r.group = group_;
    r.bookmark = bookmark_;
    r.licence = "CC-BY-4.0";
    r.authors_json = R"([{"name": "J. Ross"}])";
    r.options_json = "{}";
    r.reduction_version = "arar-1";
    r.software_version = "nobion/0.4.0";
    r.samples = {sample1_, sample2_, sample3_};
    return r;
  }

  Uuid project_, project2_, material_, sample1_, sample2_, sample3_, analysis1_, analysis2_, analysis3_, group_,
      bookmark_;
};

}  // namespace

TEST_P(PublishStoreTest, AddReleaseWritesRowSamplesAndInterpretedAges) {
  const ChangeSeq before = *store_->latest_change_seq();
  auto spec = insert("r");
  auto ia = store_->add_interpreted_age(lab_.reduction_client, {"FC-1 plateau"});
  ASSERT_TRUE(ia);
  spec.interpreted_ages = {{*ia, **store_->head(analysis1_, Kind::Tags)}};
  auto id = store_->add_release(reducer(), spec);
  ASSERT_TRUE(id) << to_string(id.error());
  EXPECT_EQ(*id, spec.uuid);
  auto row = store_->release(*id);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->name, "r");
  EXPECT_EQ((*row)->title, "Release r");
  EXPECT_EQ((*row)->state, ReleaseState::Draft);
  EXPECT_EQ((*row)->visibility, Visibility::Private);
  EXPECT_EQ((*row)->licence, "CC-BY-4.0");
  EXPECT_EQ((*row)->bookmark, bookmark_);
  EXPECT_FALSE((*row)->published);
  auto samples = store_->release_samples(*id);
  ASSERT_TRUE(samples);
  EXPECT_EQ(samples->size(), 3u);
  auto ias = store_->release_interpreted_ages(*id);
  ASSERT_TRUE(ias);
  ASSERT_EQ(ias->size(), 1u);
  EXPECT_EQ((*ias)[0].ia, *ia);
  EXPECT_EQ(*store_->latest_change_seq(), before + 2) << "the interpreted age and the release";
}

TEST_P(PublishStoreTest, FindReleaseGivesNewestVersion) {
  ASSERT_TRUE(store_->add_release(reducer(), insert("r", 1)));
  auto v2 = insert("r", 2);
  ASSERT_TRUE(store_->add_release(reducer(), v2));
  auto newest = store_->find_release("r", std::nullopt);
  ASSERT_TRUE(newest && *newest);
  EXPECT_EQ((*newest)->version, 2);
  EXPECT_EQ((*newest)->uuid, v2.uuid);
  auto first = store_->find_release("r", 1);
  ASSERT_TRUE(first && *first);
  EXPECT_EQ((*first)->version, 1);
  auto none = store_->find_release("s", std::nullopt);
  ASSERT_TRUE(none);
  EXPECT_FALSE(*none);
  auto all = store_->releases({});
  ASSERT_TRUE(all);
  EXPECT_EQ(all->size(), 2u);
  EXPECT_EQ((*all)[0].version, 1);
  auto drafts = store_->releases({ReleaseState::Draft, "r"});
  ASSERT_TRUE(drafts);
  EXPECT_EQ(drafts->size(), 2u);
}

TEST_P(PublishStoreTest, DraftToPublishedSetsPublishedUtc) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate u;
  u.release = spec.uuid;
  u.state = ReleaseState::Published;
  u.manifest_sha256 = sha256(std::string_view{"manifest"});
  ASSERT_TRUE(store_->update_release(reducer(), u));
  auto row = store_->release(spec.uuid);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ReleaseState::Published);
  EXPECT_TRUE((*row)->published);
  EXPECT_EQ((*row)->visibility, Visibility::Private);
  ASSERT_TRUE((*row)->manifest_sha256);
  EXPECT_EQ(*(*row)->manifest_sha256, sha256(std::string_view{"manifest"}));
}

TEST_P(PublishStoreTest, PublishedCannotGoPrivateOrChangeTitle) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate publish;
  publish.release = spec.uuid;
  publish.state = ReleaseState::Published;
  publish.visibility = Visibility::Public;
  ASSERT_TRUE(store_->update_release(reducer(), publish));
  ReleaseUpdate hide;
  hide.release = spec.uuid;
  hide.visibility = Visibility::Private;
  auto refused = store_->update_release(reducer(), hide);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  ReleaseUpdate draft_public;
  draft_public.release = spec.uuid;
  draft_public.state = ReleaseState::Draft;
  EXPECT_FALSE(store_->update_release(reducer(), draft_public));
}

TEST_P(PublishStoreTest, DraftVisibilityCannotChange) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate u;
  u.release = spec.uuid;
  u.visibility = Visibility::Public;
  auto refused = store_->update_release(reducer(), u);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
}

TEST_P(PublishStoreTest, WithdrawNeedsAReason) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate publish;
  publish.release = spec.uuid;
  publish.state = ReleaseState::Published;
  publish.visibility = Visibility::Public;
  ASSERT_TRUE(store_->update_release(reducer(), publish));
  ReleaseUpdate withdraw;
  withdraw.release = spec.uuid;
  withdraw.state = ReleaseState::Withdrawn;
  EXPECT_FALSE(store_->update_release(reducer(), withdraw));
  withdraw.withdrawn_reason = "wrong J";
  withdraw.visibility = Visibility::Private;
  ASSERT_TRUE(store_->update_release(reducer(), withdraw));
  auto row = store_->release(spec.uuid);
  ASSERT_TRUE(row && *row);
  EXPECT_EQ((*row)->state, ReleaseState::Withdrawn);
  EXPECT_EQ((*row)->visibility, Visibility::Private);
  EXPECT_TRUE((*row)->withdrawn);
  EXPECT_EQ((*row)->withdrawn_reason, "wrong J");
  ReleaseUpdate again;
  again.release = spec.uuid;
  again.doi = "10.1/x";
  EXPECT_FALSE(store_->update_release(reducer(), again)) << "a withdrawn release never changes";
}

TEST_P(PublishStoreTest, DeleteOnlyADraft) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ASSERT_TRUE(store_->delete_release(reducer(), spec.uuid));
  auto gone = store_->release(spec.uuid);
  ASSERT_TRUE(gone);
  EXPECT_FALSE(*gone);
  EXPECT_TRUE(store_->release_samples(spec.uuid)->empty());
  const auto published = insert("p");
  ASSERT_TRUE(store_->add_release(reducer(), published));
  ReleaseUpdate u;
  u.release = published.uuid;
  u.state = ReleaseState::Published;
  ASSERT_TRUE(store_->update_release(reducer(), u));
  auto refused = store_->delete_release(reducer(), published.uuid);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
}

TEST_P(PublishStoreTest, EmbargoOnProjectCoversItsSamplesAndAnalyses) {
  const ChangeSeq before = *store_->latest_change_seq();
  auto e = store_->add_embargo(reducer(), {{EmbargoScopeType::Project, project_}, std::nullopt, "paper in review"});
  ASSERT_TRUE(e) << to_string(e.error());
  EXPECT_EQ(*store_->latest_change_seq(), before + 1);
  const UtcTime now = at("2026-10-18T12:00:00Z");
  const std::vector<Uuid> samples{sample1_, sample2_, sample3_};
  auto es = store_->embargoed_samples(samples, now);
  ASSERT_TRUE(es);
  EXPECT_EQ(*es, (std::vector<Uuid>{std::min(sample1_, sample2_), std::max(sample1_, sample2_)}));
  const std::vector<Uuid> analyses{analysis1_, analysis2_, analysis3_};
  auto ea = store_->embargoed_analyses(analyses, now);
  ASSERT_TRUE(ea);
  EXPECT_EQ(ea->size(), 2u);
  EXPECT_EQ(std::count(ea->begin(), ea->end(), analysis3_), 0);
  auto live = store_->embargoes(true, now);
  ASSERT_TRUE(live);
  ASSERT_EQ(live->size(), 1u);
  EXPECT_EQ((*live)[0].scope.type, EmbargoScopeType::Project);
  EXPECT_EQ((*live)[0].reason, "paper in review");
  EXPECT_FALSE((*live)[0].lifted);

  ASSERT_TRUE(store_->lift_embargo(reducer(), *e));
  EXPECT_TRUE(store_->embargoed_samples(samples, now)->empty());
  EXPECT_TRUE(store_->embargoed_analyses(analyses, now)->empty());
  EXPECT_TRUE(store_->embargoes(true, now)->empty());
  EXPECT_EQ(store_->embargoes(false, now)->size(), 1u);
  auto twice = store_->lift_embargo(reducer(), *e);
  ASSERT_FALSE(twice);
  EXPECT_EQ(twice.error().kind, ErrorKind::Config);
}

TEST_P(PublishStoreTest, EmbargoOnAnalysisCoversOnlyIt) {
  ASSERT_TRUE(store_->add_embargo(reducer(), {{EmbargoScopeType::Analysis, analysis1_}, std::nullopt, std::nullopt}));
  const UtcTime now = at("2026-10-18T12:00:00Z");
  const std::vector<Uuid> samples{sample1_, sample2_, sample3_}, analyses{analysis1_, analysis2_, analysis3_};
  EXPECT_TRUE(store_->embargoed_samples(samples, now)->empty());
  auto ea = store_->embargoed_analyses(analyses, now);
  ASSERT_TRUE(ea);
  EXPECT_EQ(*ea, std::vector<Uuid>{analysis1_});
}

TEST_P(PublishStoreTest, EmbargoExpiresAtUntil) {
  const UtcTime until = at("2027-01-01T00:00:00Z");
  ASSERT_TRUE(store_->add_embargo(reducer(), {{EmbargoScopeType::Sample, sample1_}, until, std::nullopt}));
  const std::vector<Uuid> one{sample1_};
  EXPECT_EQ(store_->embargoed_samples(one, at("2026-12-31T23:59:59Z"))->size(), 1u);
  EXPECT_TRUE(store_->embargoed_samples(one, until)->empty());
  EXPECT_EQ(store_->embargoes(true, at("2026-12-31T23:59:59Z"))->size(), 1u);
  EXPECT_TRUE(store_->embargoes(true, until)->empty());
}

TEST_P(PublishStoreTest, ExternalIdUniquePerSchemeAndValue) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ExternalIdInsert id;
  id.entity_type = "release";
  id.entity = spec.uuid;
  id.scheme = "doi";
  id.value = "10.1234/abcd";
  id.registrar = "datacite-test";
  id.state = "draft";
  id.request_sha256 = sha256(std::string_view{"request"});
  id.response_json = R"({"data": {"id": "10.1234/abcd"}})";
  auto first = store_->add_external_id(reducer(), id);
  ASSERT_TRUE(first) << to_string(first.error());
  auto rows = store_->external_ids("release", spec.uuid);
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  EXPECT_EQ((*rows)[0].value, "10.1234/abcd");
  EXPECT_EQ((*rows)[0].state, "draft");
  EXPECT_EQ((*rows)[0].request_sha256, sha256(std::string_view{"request"}));
  EXPECT_FALSE(store_->add_external_id(reducer(), id)) << "same scheme and value twice";
}

TEST_P(PublishStoreTest, InterpretedAgesListEveryOneWithItsHead) {
  auto a = store_->add_interpreted_age(lab_.reduction_client, {"B plateau"});
  auto b = store_->add_interpreted_age(lab_.reduction_client, {"A mean", lab_.identifier});
  ASSERT_TRUE(a && b);
  auto uow = *store_->begin(reducer());
  InterpretedAgeValue v;
  v.age = 28.2;
  v.members = {{analysis1_, std::nullopt, std::nullopt, std::nullopt}};
  ASSERT_TRUE(uow->add_revision(*b, Kind::InterpretedAge, v, std::nullopt));
  auto outcome = uow->commit(ChangesetKind::Reduction, "ia");
  ASSERT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
  auto rows = store_->interpreted_ages();
  ASSERT_TRUE(rows) << to_string(rows.error());
  ASSERT_EQ(rows->size(), 2u);
  EXPECT_EQ((*rows)[0].name, "A mean");
  EXPECT_EQ((*rows)[0].identifier_text, "66573");
  EXPECT_TRUE((*rows)[0].head);
  EXPECT_EQ((*rows)[1].name, "B plateau");
  EXPECT_FALSE((*rows)[1].head);
}

TEST_P(PublishStoreTest, AnalysisSummaryCarriesTheRecordHash) {
  auto item = analysis_item(lab_, 3, series(1), series(0), "66574");
  std::get<AnalysisIngest>(item.body).record_sha256 = sha256(std::string_view{"record"});
  ASSERT_TRUE(store_->ingest(item));
  auto view = store_->load_analysis(std::get<AnalysisIngest>(item.body).analysis);
  ASSERT_TRUE(view && *view);
  ASSERT_TRUE((*view)->summary.record_sha256);
  EXPECT_EQ(*(*view)->summary.record_sha256, sha256(std::string_view{"record"}));
  auto plain = store_->load_analysis(analysis1_);
  ASSERT_TRUE(plain && *plain);
  EXPECT_FALSE((*plain)->summary.record_sha256);
}

// ---------------------------------------------------------------- as of a bookmark (4.3)

TEST_P(PublishStoreTest, LoadAnalysisAtBookmarkIgnoresLaterHeadMoves) {
  const Uuid old_head = **store_->head(analysis1_, Kind::Intercepts);
  auto uow = *store_->begin(reducer());
  InterceptRow row;
  row.isotope = "Ar40";
  row.detector = "H1";
  row.value = 200.0;
  ASSERT_TRUE(uow->add_revision(analysis1_, Kind::Intercepts, Intercepts{row}, old_head));
  auto outcome = uow->commit(ChangesetKind::Reduction, "refit");
  ASSERT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
  ASSERT_NE(**store_->head(analysis1_, Kind::Intercepts), old_head);

  auto at_bookmark = store_->load_analysis_at(analysis1_, bookmark_);
  ASSERT_TRUE(at_bookmark) << to_string(at_bookmark.error());
  ASSERT_TRUE(*at_bookmark);
  const auto& view = **at_bookmark;
  EXPECT_EQ(view.summary.uuid, analysis1_);
  const auto head_of = [&](Kind kind) {
    for (const auto& h : view.heads)
      if (h.kind == kind) return h.revision;
    return Uuid{};
  };
  EXPECT_EQ(head_of(Kind::Intercepts), old_head);
  ASSERT_TRUE(view.payloads.contains(Kind::Intercepts));
  EXPECT_EQ(std::get<Intercepts>(view.payloads.at(Kind::Intercepts))[0].value, 100.5);
  auto now = store_->load_analysis(analysis1_);
  ASSERT_TRUE(now && *now);
  EXPECT_EQ(std::get<Intercepts>((*now)->payloads.at(Kind::Intercepts))[0].value, 200.0);

  auto other = store_->load_analysis_at(analysis1_, Uuid::v7());
  ASSERT_TRUE(other);
  EXPECT_FALSE(*other) << "a bookmark without an entry for the analysis";
}

TEST_P(PublishStoreTest, ResolveRefsAtBookmarkUsesPinnedFluxRevision) {
  // An analysis on the irradiated identifier, whose position has a flux value.
  const auto item = analysis_item(lab_, 2, series(1), series(0), "66573");
  ASSERT_TRUE(store_->ingest(item));
  const Uuid analysis = std::get<AnalysisIngest>(item.body).analysis;
  RefObjectSpec spec;
  spec.type = RefType::FluxPosition;
  spec.key = "NM-300/A/1";
  spec.position = lab_.position;
  const Uuid flux = *store_->add_ref_object(lab_.reduction_client, spec);
  const auto publish = [&](double j) {
    auto uow = *store_->begin(reducer());
    FluxValue v;
    v.j = j;
    EXPECT_TRUE(uow->add_revision(flux, Kind::RefValue, RevisionPayload{RefPayload{v}},
                                  *store_->head(flux, Kind::RefValue)));
    auto outcome = uow->commit(ChangesetKind::Reference, "flux");
    EXPECT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
  };
  publish(0.001);
  const Uuid pinned = **store_->head(flux, Kind::RefValue);
  // A bookmark with explicit entries: the analysis's heads and the flux at its revision.
  std::vector<HeadInfo> entries = *store_->heads(analysis);
  entries.push_back(HeadInfo{flux, Kind::RefValue, pinned, 1});
  BookmarkSpec bm{"release r v2", std::nullopt, std::nullopt, group_};
  bm.entries = entries;
  const Uuid bookmark = *store_->create_bookmark(reducer(), bm);
  EXPECT_EQ(store_->bookmark_heads(bookmark)->size(), entries.size());
  publish(0.002);
  ASSERT_NE(**store_->head(flux, Kind::RefValue), pinned);

  auto at = store_->resolve_refs_at(analysis, bookmark);
  ASSERT_TRUE(at) << to_string(at.error());
  auto head = store_->resolve_refs(analysis, RefPolicy{});
  ASSERT_TRUE(head);
  const auto find = [](const RefResolution& r, Uuid object) -> const ResolvedRef* {
    for (const auto& ref : r.refs)
      if (ref.ref_object == object) return &ref;
    return nullptr;
  };
  ASSERT_NE(find(*at, flux), nullptr);
  EXPECT_EQ(find(*at, flux)->revision, pinned);
  EXPECT_TRUE(find(*at, flux)->pinned);
  ASSERT_NE(find(*head, flux), nullptr);
  EXPECT_NE(find(*head, flux)->revision, pinned);
}

TEST_P(PublishStoreTest, PublishedManifestHashNeverChanges) {
  const auto spec = insert("r");
  ASSERT_TRUE(store_->add_release(reducer(), spec));
  ReleaseUpdate publish;
  publish.release = spec.uuid;
  publish.state = ReleaseState::Published;
  publish.manifest_sha256 = sha256(std::string_view{"one"});
  ASSERT_TRUE(store_->update_release(reducer(), publish));
  ReleaseUpdate other;
  other.release = spec.uuid;
  other.manifest_sha256 = sha256(std::string_view{"two"});
  auto refused = store_->update_release(reducer(), other);
  ASSERT_FALSE(refused);
  EXPECT_EQ(refused.error().kind, ErrorKind::Config);
  ReleaseUpdate same;
  same.release = spec.uuid;
  same.manifest_sha256 = sha256(std::string_view{"one"});
  same.visibility = Visibility::Public;
  EXPECT_TRUE(store_->update_release(reducer(), same)) << "the same hash is no change";
  EXPECT_EQ(*(*store_->release(spec.uuid))->manifest_sha256, sha256(std::string_view{"one"}));
}

TEST_P(PublishStoreTest, EmbargoLookupsTakeManyUuids) {
  std::vector<Uuid> many;
  many.reserve(40001);
  for (int i = 0; i < 40000; ++i) many.push_back(Uuid::v7());
  many.push_back(sample1_);
  ASSERT_TRUE(store_->add_embargo(reducer(), {{EmbargoScopeType::Sample, sample1_}, std::nullopt, std::nullopt}));
  auto es = store_->embargoed_samples(many, at("2026-10-18T12:00:00Z"));
  ASSERT_TRUE(es) << to_string(es.error());
  EXPECT_EQ(*es, std::vector<Uuid>{sample1_});
  many.push_back(analysis1_);
  auto ea = store_->embargoed_analyses(many, at("2026-10-18T12:00:00Z"));
  ASSERT_TRUE(ea) << to_string(ea.error());
  EXPECT_EQ(*ea, std::vector<Uuid>{analysis1_});
}

TEST_P(PublishStoreTest, ResolveRefsAtDropsReferencesMadeAfterTheBookmark) {
  const auto item = analysis_item(lab_, 2, series(1), series(0), "66573");
  ASSERT_TRUE(store_->ingest(item));
  const Uuid analysis = std::get<AnalysisIngest>(item.body).analysis;
  const std::vector<HeadInfo> heads = *store_->heads(analysis);
  BookmarkSpec bm{"before the flux", std::nullopt, std::nullopt, group_};
  bm.entries = heads;  // no reference pinned: none existed
  const Uuid bookmark = *store_->create_bookmark(reducer(), bm);
  RefObjectSpec spec;
  spec.type = RefType::FluxPosition;
  spec.key = "NM-300/A/1";
  spec.position = lab_.position;
  const Uuid flux = *store_->add_ref_object(lab_.reduction_client, spec);
  auto uow = *store_->begin(reducer());
  FluxValue v;
  v.j = 0.001;
  ASSERT_TRUE(uow->add_revision(flux, Kind::RefValue, RevisionPayload{RefPayload{v}}, std::nullopt));
  ASSERT_TRUE(uow->commit(ChangesetKind::Reference, "flux"));
  EXPECT_FALSE(store_->resolve_refs(analysis, RefPolicy{})->refs.empty()) << "at head the flux resolves";
  auto at_bookmark = store_->resolve_refs_at(analysis, bookmark);
  ASSERT_TRUE(at_bookmark);
  EXPECT_TRUE(at_bookmark->refs.empty()) << "as of the bookmark there was no flux";
}

INSTANTIATE_TEST_SUITE_P(Engines, PublishStoreTest, ::testing::ValuesIn(engines()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });

// ---------------------------------------------------------------- USGS Geochron view (spec 9.2)

namespace {

// The arar_constants of a stored interpreted age, under "preferred" as a
// nested legacy document has them.
constexpr const char* kPlateauDoc =
    R"({"preferred": {"arar_constants": {"lambda_k": 5.463e-10, "lambda_k_err": 1.0e-11,)"
    R"( "atm4036": 298.56, "atm4036_err": 0.31}}})";

// pub_geochron_ar40ar39 is PostgreSQL only: a sample with a location, one
// analysis, and interpreted ages of it released as the tests choose.
class GeochronViewTest : public PublishStoreTest {
 protected:
  void SetUp() override {
    PublishStoreTest::SetUp();
    SampleSpec spec{"RC-1", project_, material_};
    spec.lat = 33.5;
    spec.lon = -106.25;
    spec.elevation = 1520.0;
    spec.igsn = "IEJR00001";
    spec.unit = "Bandelier Tuff";
    spec.lithology = "rhyolite";
    spec.location = "Valles caldera rim";
    located_ = *store_->add_sample(lab_.reduction_client, spec);
    analysis_ = ingest("70004", located_);
    located_group_ = *store_->create_group(reducer(), "release:geochron", {analysis_});
    located_bookmark_ =
        *store_->create_bookmark(reducer(), BookmarkSpec{"geochron", std::nullopt, std::nullopt, located_group_});
  }

  Uuid age_of(const std::vector<Uuid>& members, const std::string& kind = "Plateau",
              const std::string& doc = kPlateauDoc) {
    const Uuid ia = *store_->add_interpreted_age(lab_.reduction_client, {"RC-1 " + kind});
    InterpretedAgeValue v;
    v.age = 31.92;
    v.age_err = 0.03;
    v.age_kind = kind;
    v.kca = 19.2;
    v.kca_err = 0.4;
    v.mswd = 2.56;
    v.nanalyses = 5;
    v.doc_json = doc;
    for (const Uuid& member : members) v.members.push_back({member, std::nullopt, std::nullopt, std::nullopt});
    auto uow = *store_->begin(reducer());
    EXPECT_TRUE(uow->add_revision(ia, Kind::InterpretedAge, v, std::nullopt));
    auto outcome = uow->commit(ChangesetKind::Reduction, "ia");
    EXPECT_TRUE(outcome && std::holds_alternative<Committed>(*outcome));
    return ia;
  }

  ReleaseInsert located_release(const std::string& name, const std::vector<Uuid>& ages, int version = 1) const {
    ReleaseInsert r = insert(name, version);
    r.group = located_group_;
    r.bookmark = located_bookmark_;
    r.samples = {located_};
    for (const Uuid& ia : ages) r.interpreted_ages.emplace_back(ia, **store_->head(ia, Kind::InterpretedAge));
    return r;
  }

  void publish(const ReleaseInsert& r, Visibility visibility = Visibility::Public) {
    auto id = store_->add_release(reducer(), r);
    ASSERT_TRUE(id) << to_string(id.error());
    ReleaseUpdate u;
    u.release = r.uuid;
    u.state = ReleaseState::Published;
    u.visibility = visibility;
    u.manifest_sha256 = sha256(std::string_view{"manifest"});
    auto updated = store_->update_release(reducer(), u);
    ASSERT_TRUE(updated) << to_string(updated.error());
  }

  std::vector<pd::Row> rows() {
    auto db = pd::Db::open(StoreConfig{db_->url(), false});
    EXPECT_TRUE(db) << (db ? "" : to_string(db.error()));
    if (!db) return {};
    auto out = (*db)->select("SELECT * FROM pub_geochron_ar40ar39 ORDER BY id");
    EXPECT_TRUE(out) << (out ? "" : to_string(out.error()));
    return out ? *out : std::vector<pd::Row>{};
  }

  Uuid located_, analysis_, located_group_, located_bookmark_;
};

}  // namespace

TEST_P(GeochronViewTest, APublishedPlateauIsAParentRowInUsgsTerms) {
  const Uuid ia = age_of({analysis_});
  auto release = located_release("r", {ia});
  release.title = "Valles caldera ages";
  publish(release);

  const auto got = rows();
  ASSERT_EQ(got.size(), 1u);
  const auto& r = got[0];
  EXPECT_EQ(pd::to_std(r.value("id")), release.uuid.str() + "_" + ia.str());
  EXPECT_EQ(pd::to_std(r.value("sample_id")), "RC-1");
  EXPECT_EQ(pd::to_std(r.value("igsn")), "IEJR00001");
  EXPECT_DOUBLE_EQ(r.value("lat").toDouble(), 33.5);
  EXPECT_DOUBLE_EQ(r.value("long").toDouble(), -106.25);
  EXPECT_DOUBLE_EQ(r.value("elevation").toDouble(), 1520.0);
  EXPECT_EQ(pd::to_std(r.value("fieldname")), "rhyolite");
  EXPECT_EQ(pd::to_std(r.value("geologicalunit")), "Bandelier Tuff");
  EXPECT_EQ(pd::to_std(r.value("localitydesc")), "Valles caldera rim");
  EXPECT_EQ(pd::to_std(r.value("samplematerial")), "sanidine");
  EXPECT_EQ(pd::to_std(r.value("argon_id")), "70004");
  EXPECT_DOUBLE_EQ(r.value("age").toDouble(), 31.92);
  EXPECT_DOUBLE_EQ(r.value("ageunc").toDouble(), 0.03);
  EXPECT_DOUBLE_EQ(r.value("mswd").toDouble(), 2.56);
  EXPECT_EQ(r.value("nofanalyses").toInt(), 5);
  EXPECT_DOUBLE_EQ(r.value("kcaratio").toDouble(), 19.2);
  EXPECT_DOUBLE_EQ(r.value("kcaratiounc").toDouble(), 0.4);
  EXPECT_EQ(pd::to_std(r.value("ageintpntype")), "plateau");
  EXPECT_EQ(pd::to_std(r.value("ageintpnclass")), "not specified");
  EXPECT_EQ(pd::to_std(r.value("analysistype")), "Incremental heating");
  EXPECT_EQ(pd::to_std(r.value("instrumentmethod")), "Noble gas mass spectrometer");
  EXPECT_DOUBLE_EQ(r.value("decayconsttotal").toDouble(), 5.463);
  EXPECT_NEAR(r.value("decayconttotalunc").toDouble(), 0.1, 1e-12);
  EXPECT_DOUBLE_EQ(r.value("initratioar4036").toDouble(), 298.56);
  EXPECT_DOUBLE_EQ(r.value("initratioar4036unc").toDouble(), 0.31);
  EXPECT_EQ(pd::to_std(r.value("citation")), "Valles caldera ages");
  EXPECT_EQ(pd::to_std(r.value("publication_geodatum")), "WGS84");
  EXPECT_EQ(pd::to_std(r.value("release_name")), "r");
  EXPECT_FALSE(r.value("geom").isNull());
}

TEST_P(GeochronViewTest, AgeKindsTakeUsgsVocabulary) {
  const std::map<std::string, std::string> kinds{{"Plateau", "plateau"},
                                                 {"Weighted Mean", "weighted mean"},
                                                 {"Isochron", "isochron"},
                                                 {"Integrated", "total gas"},
                                                 {"Something else", "not specified"}};
  std::vector<Uuid> ages;
  ages.reserve(kinds.size());
  for (const auto& [kind, usgs] : kinds) ages.push_back(age_of({analysis_}, kind));
  publish(located_release("r", ages));

  std::set<std::string> seen;
  for (const auto& r : rows()) {
    const std::string type = pd::to_std(r.value("ageintpntype"));
    seen.insert(type);
    if (type == "isochron") {
      EXPECT_TRUE(r.value("initratioar4036").isNull()) << "an isochron's ratio is calculated";
    }
  }
  for (const auto& [kind, usgs] : kinds) EXPECT_TRUE(seen.contains(usgs)) << usgs;
}

// A legacy document may hold anything where a constant belongs; one bad
// value must not fail every query of the view.
TEST_P(GeochronViewTest, AConstantThatIsNotANumberIsNull) {
  const Uuid flat = age_of({analysis_}, "Plateau", R"({"arar_constants": {"lambda_k": 5.543e-10}})");
  const Uuid bad = age_of({analysis_}, "Weighted Mean", R"({"arar_constants": {"lambda_k": "n/a", "atm4036": null}})");
  publish(located_release("r", {flat, bad}));

  const auto got = rows();
  ASSERT_EQ(got.size(), 2u);
  for (const auto& r : got) {
    if (pd::to_std(r.value("ageintpntype")) == "plateau") {
      EXPECT_DOUBLE_EQ(r.value("decayconsttotal").toDouble(), 5.543);
    } else {
      EXPECT_TRUE(r.value("decayconsttotal").isNull());
      EXPECT_TRUE(r.value("initratioar4036").isNull());
    }
  }
}

TEST_P(GeochronViewTest, OnlyAPublishedPublicReleaseIsServed) {
  const Uuid ia = age_of({analysis_});
  auto draft = located_release("draft", {ia});
  ASSERT_TRUE(store_->add_release(reducer(), draft));
  publish(located_release("private", {ia}), Visibility::Private);
  EXPECT_TRUE(rows().empty());
}

TEST_P(GeochronViewTest, AnEmbargoHidesTheAge) {
  publish(located_release("r", {age_of({analysis_})}));
  ASSERT_EQ(rows().size(), 1u);

  auto e = store_->add_embargo(reducer(), {{EmbargoScopeType::Analysis, analysis_}, std::nullopt, "in review"});
  ASSERT_TRUE(e) << to_string(e.error());
  EXPECT_TRUE(rows().empty()) << "a member under embargo leaks through no age";
  ASSERT_TRUE(store_->lift_embargo(reducer(), *e));
  EXPECT_EQ(rows().size(), 1u);

  ASSERT_TRUE(store_->add_embargo(reducer(), {{EmbargoScopeType::Project, project_}, std::nullopt, "in review"}));
  EXPECT_TRUE(rows().empty());
}

// Spec R4: every member of the age has to be published with it.
TEST_P(GeochronViewTest, AnAgeWithAMemberOutsideTheReleaseIsNotServed) {
  // Of the released sample itself: only the member rule can hide the age.
  const Uuid outside = ingest("70005", located_);
  publish(located_release("r", {age_of({analysis_, outside})}));
  EXPECT_TRUE(rows().empty());
}

TEST_P(GeochronViewTest, ASampleWithNoLocationIsNotServed) {
  auto release = insert("r");
  release.samples = {sample1_};
  const Uuid ia = age_of({analysis1_});
  release.interpreted_ages = {{ia, **store_->head(ia, Kind::InterpretedAge)}};
  publish(release);
  EXPECT_TRUE(rows().empty()) << "USGS Geochron requires a location";
}

TEST_P(GeochronViewTest, EachReleaseNameServesItsNewestVersion) {
  const Uuid ia = age_of({analysis_});
  const auto v1 = located_release("r", {ia}, 1);
  publish(v1);
  auto v2 = located_release("r", {ia}, 2);
  v2.supersedes = v1.uuid;
  publish(v2);
  auto other = located_release("other", {ia});
  publish(other);

  const auto got = rows();
  ASSERT_EQ(got.size(), 2u) << "one age, two release names";
  std::set<std::string> ids;
  for (const auto& r : got) {
    ids.insert(pd::to_std(r.value("id")));
    if (pd::to_std(r.value("release_name")) == "r") {
      EXPECT_EQ(r.value("release_version").toInt(), 2);
    }
  }
  EXPECT_EQ(ids.size(), 2u) << "pygeoapi needs a unique id";
}

namespace {
std::vector<std::string> postgres_only() {
  std::vector<std::string> e;
  if (!pg_url().empty()) e.emplace_back("pg");
  return e;
}
}  // namespace

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(GeochronViewTest);
INSTANTIATE_TEST_SUITE_P(Engines, GeochronViewTest, ::testing::ValuesIn(postgres_only()),
                         [](const ::testing::TestParamInfo<std::string>& i) { return i.param; });
