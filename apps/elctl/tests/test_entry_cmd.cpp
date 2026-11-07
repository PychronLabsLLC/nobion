// elctl entry: sample and package entry driven through elctl::run against a
// file-backed SQLite store (entry spec, section 10).

#include <gtest/gtest.h>

#include "elctl_fixture.hpp"
#include "entry.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef PYCHRON_ELCTL_HAS_STORE

TEST(EntryCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"entry", "samples", "list", "--db", "sqlite::memory:"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
}

#else

#include <string>
#include <vector>

#include "nobion/persistence/store.hpp"

namespace ps = nobion::persistence;

namespace {

class EntryCmd : public elctl::testing::ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    db_ = "sqlite:" + path("store.db").string();
    auto store = ps::open_store(ps::StoreConfig{db_, true});  // elctl entry never migrates
    ASSERT_TRUE(store) << nobion::to_string(store.error());
  }

  Outcome entry(std::vector<std::string> args) const {
    args.insert(args.begin(), "entry");
    args.insert(args.end(), {"--db", db_, "--user", "tester"});
    return run_raw(std::move(args));
  }

  std::string db_;
};

constexpr const char* kSamples =
    "sample,project,pi,material,grainsize,lat,lon\n"
    "FC-2,Irradiation-NM-001,NMGRL Lab,sanidine,,,\n"
    "bt-1,Alpha,\"Ross, J\",biotite,20-40,34.1,-106.9\n"
    "bt-2,Alpha,\"Ross, J\",biotite,20-40,,\n";

}  // namespace

TEST_F(EntryCmd, HelpAndUsage) {
  EXPECT_EQ(run_raw({"entry", "help"}).code, elctl::kOk);
  EXPECT_TRUE(contains(run_raw({"entry", "help"}).out, "identifiers generate"));
  EXPECT_EQ(entry({"samples", "nothing"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"entry", "samples", "list"}).code, elctl::kUsage);  // no --db
}

TEST_F(EntryCmd, SamplesTemplateAndDryRun) {
  ASSERT_EQ(entry({"samples", "template", path("t.csv").string()}).code, elctl::kOk);
  ASSERT_TRUE(std::filesystem::exists(path("t.csv")));

  // A lab's name is a PI name as it is written ("NMGRL Lab"); one with a
  // digit in it is not a person's, and is an organisation's when the sheet
  // says so (there is no list of allowed names, PI spec 6.1).
  std::string sheet = kSamples;
  const std::string lab = "NMGRL Lab";
  sheet.replace(sheet.find(lab), lab.size(), "Lab 7");
  write("s.csv", sheet);
  Outcome o = entry({"samples", "import", path("s.csv").string(), "--dry-run"});
  EXPECT_EQ(o.code, elctl::kFailed) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "line 2 error FC-2")) << o.out;
  write("s.csv", "sample,project,pi,material,principal_investigator_kind\n"
                 "FC-2,Irradiation-NM-001,Lab 7,sanidine,organisation\n"
                 "bt-1,Alpha,\"Ross, J\",biotite,\n"
                 "bt-2,Alpha,\"Ross, J\",biotite,\n");
  o = entry({"samples", "import", path("s.csv").string(), "--dry-run"});
  EXPECT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "3 to create")) << o.out;
  EXPECT_EQ(entry({"samples", "list"}).out, "");  // nothing written
}

TEST_F(EntryCmd, ImportThenReimport) {
  write("s.csv", kSamples);
  Outcome o = entry({"samples", "import", path("s.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "new project Alpha (Ross, J)")) << o.out;
  o = entry({"samples", "import", path("s.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "0 to create, 0 differing, 3 stored")) << o.out;
  o = entry({"samples", "list", "--project", "Alpha"});
  EXPECT_TRUE(contains(o.out, "bt-1\tAlpha\tJ Ross\tbiotite\t20-40")) << o.out;
  EXPECT_FALSE(contains(o.out, "FC-2"));
}

// `settings set` takes the keys this version reads; another, the former
// allowed-names list among them (PI spec 6.1), is refused and nothing is
// written.
TEST_F(EntryCmd, SettingsSetRefusesAnUnknownKey) {
  ASSERT_EQ(entry({"settings", "set", "package_prefix", "P-"}).code, elctl::kOk);
  EXPECT_TRUE(contains(entry({"settings", "show"}).out, R"("package_prefix": "P-")"));
  for (const char* key : {"pi_names_allowed", "no_such_key"}) {
    const Outcome o = entry({"settings", "set", key, "[\"Lab 7\"]"});
    EXPECT_EQ(o.code, elctl::kUsage) << key << o.out << o.err;
    EXPECT_TRUE(contains(o.err, std::string("unknown key '") + key + "'")) << o.err;
    EXPECT_FALSE(contains(entry({"settings", "show"}).out, key));
  }
}

// A sheet that names a PI with namesakes by name alone stops: the namesakes
// are listed with what tells them apart, and nothing is written. An ORCID or
// uuid column says which.
TEST_F(EntryCmd, ANamesakeStopsTheImport) {
  write("s.csv", kSamples);
  ASSERT_EQ(entry({"samples", "import", path("s.csv").string()}).code, elctl::kOk);
  write("n.csv", "sample,project,pi,material\nbt-9,Alpha,\"Ross, Jake\",biotite\n");
  for (const bool dry : {true, false}) {
    std::vector<std::string> args{"samples", "import", path("n.csv").string()};
    if (dry) args.emplace_back("--dry-run");
    const Outcome o = entry(args);
    EXPECT_EQ(o.code, elctl::kFailed) << o.out << o.err;
    EXPECT_TRUE(contains(o.out, "line 2 ask bt-9")) << o.out;
    EXPECT_TRUE(contains(o.out, "principal investigator Ross, Jake (line 2) has namesakes:")) << o.out;
    EXPECT_TRUE(contains(o.out, "  J Ross\t")) << o.out;
    EXPECT_TRUE(contains(o.err, "principal_investigator_orcid")) << o.err;
    EXPECT_TRUE(contains(o.err, "nothing was written")) << o.err;
  }
  EXPECT_FALSE(contains(entry({"samples", "list"}).out, "bt-9"));

  // the uuid listed says which
  const Outcome asked = entry({"samples", "import", path("n.csv").string(), "--dry-run"});
  const auto at = asked.out.find("  J Ross\t");
  ASSERT_NE(at, std::string::npos);
  const auto end = asked.out.find('\n', at);
  const std::string uuid = asked.out.substr(asked.out.rfind('\t', end) + 1, end - asked.out.rfind('\t', end) - 1);
  write("u.csv", "sample,project,pi,material,principal_investigator_uuid\nbt-9,Alpha,\"Ross, Jake\",biotite," + uuid + "\n");
  const Outcome o = entry({"samples", "import", path("u.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "1 to create")) << o.out;
  EXPECT_FALSE(contains(o.out, "new principal investigator")) << o.out;
  EXPECT_TRUE(contains(entry({"samples", "list", "--project", "Alpha"}).out, "bt-9\tAlpha\tJ Ross")) << o.out;
}

TEST_F(EntryCmd, PackagePositionsIdentifiers) {
  write("s.csv", kSamples);
  ASSERT_EQ(entry({"samples", "import", path("s.csv").string()}).code, elctl::kOk);
  write("24-hole.txt", "circle,0.0175\n0,0\n1,0\n2,0\n3,0\n");
  ASSERT_EQ(entry({"holders", "import", path("24-hole.txt").string()}).code, elctl::kOk);

  // A plain package needs no chronology or reactor.
  Outcome o = entry({"package", "add", "P-1", "--kind", "package", "--levels", "A"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  o = entry({"package", "show", "P-1"});
  EXPECT_TRUE(contains(o.out, "P-1 (package)")) << o.out;

  // An irradiation does.
  o = entry({"package", "add", "NM-001", "--levels", "A-B", "--holder", "24-hole"});
  EXPECT_EQ(o.code, elctl::kFailed) << o.out;
  EXPECT_TRUE(contains(o.out, "needs a reactor")) << o.out;
  write("chron.txt", "1.0,2026-09-01 08:00:00,2026-09-01 16:00:00\n");
  o = entry({"package", "add", "NM-001", "--levels", "A-B", "--holder", "24-hole", "--z", "0.5", "--reactor", "Triga",
             "--chronology", path("chron.txt").string(), "--tz", "America/Denver"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "8 h")) << o.out;

  write("p.csv",
        "level,position,sample,project,principal_investigator,material,grainsize,packet\n"
        "A,1,FC-2,Irradiation-NM-001,NMGRL Lab,sanidine,,P1\n"
        "A,3,bt-1,Alpha,\"Ross, J\",biotite,20-40,P2\n"
        "B,2,bt-2,,,,,\n");
  o = entry({"positions", "import", "NM-001", path("p.csv").string()});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;

  o = entry({"identifiers", "generate", "NM-001", "--dry-run"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  EXPECT_TRUE(contains(o.out, "A1 FC-2 - -> 1\nA3 bt-1 - -> 2\nB2 bt-2 - -> 3\n")) << o.out;
  EXPECT_TRUE(contains(o.out, "warning level B has no monitor")) << o.out;
  o = entry({"identifiers", "generate", "NM-001"});
  ASSERT_EQ(o.code, elctl::kOk) << o.out << o.err;
  o = entry({"package", "show", "NM-001", "--csv"});
  EXPECT_TRUE(contains(o.out, "A,1,1,FC-2,")) << o.out;
  EXPECT_TRUE(contains(o.out, "B,2,3,bt-2,")) << o.out;
  // Numbered positions are not numbered again.
  o = entry({"identifiers", "generate", "NM-001"});
  EXPECT_TRUE(contains(o.out, "0 identifiers")) << o.out;

  ASSERT_EQ(entry({"package", "set-kind", "NM-001", "package"}).code, elctl::kOk);
  EXPECT_TRUE(contains(entry({"package", "show", "NM-001"}).out, "NM-001 (package)"));
}

TEST_F(EntryCmd, PositionsImportErrorsWriteNothing) {
  ASSERT_EQ(entry({"package", "add", "P-1", "--kind", "package", "--levels", "A"}).code, elctl::kOk);
  write("p.csv", "level,position,sample\nA,1,nope\nZ,1,x\n");
  const Outcome o = entry({"positions", "import", "P-1", path("p.csv").string()});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.out, "line 2: no sample nope")) << o.out;
  EXPECT_TRUE(contains(o.out, "line 3: no level 'Z'")) << o.out;
}

TEST_F(EntryCmd, SeedAppliesAFileAndDryRunWritesNothing) {
  const auto file = path("seed.toml");
  {
    std::ofstream out(file);
    out << "project = \"references\"\n[[samples]]\nidentifier = \"a\"\nanalysis_type = \"air\"\n"
           "sample = \"air\"\nmaterial = \"air\"\n[reactors.Triga]\nK4039 = [0.00873, 0.00017]\n";
  }
  auto o = entry({"seed", file.string(), "--dry-run"});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "would be seeded 1 project, 1 material, 1 sample, 1 identifier, 1 reactor")) << o.out;
  EXPECT_EQ(entry({"samples", "list", "--project", "references"}).out, "");

  o = entry({"seed", file.string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "seeded 1 project, 1 material, 1 sample, 1 identifier, 1 reactor")) << o.out;
  EXPECT_TRUE(contains(entry({"samples", "list", "--project", "references"}).out, "air\treferences"));

  o = entry({"seed", file.string()});
  EXPECT_EQ(o.code, 0) << o.err;
  EXPECT_TRUE(contains(o.out, "seed: nothing to add (5 already there)")) << o.out;

  // entry seed never makes or migrates a schema: a database without one is refused.
  const std::string bare = "sqlite:" + path("bare.db").string();
  auto refused = run_raw({"entry", "seed", file.string(), "--db", bare, "--user", "tester"});
  EXPECT_EQ(refused.code, 2);
  EXPECT_TRUE(contains(refused.err, "the database could not be opened")) << refused.err;

  EXPECT_EQ(entry({"seed", path("missing.toml").string()}).code, 2);
  EXPECT_EQ(entry({"seed"}).code, 2);
  EXPECT_EQ(entry({"seed", file.string(), "extra"}).code, 2);
  { std::ofstream(file) << "project = \n"; }
  o = entry({"seed", file.string()});
  EXPECT_EQ(o.code, 2);
  EXPECT_TRUE(contains(o.err, "seed.toml")) << o.err;
}

#endif
