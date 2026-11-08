// elctl db: whether a store's schema is current, and bringing one up to date.
#include <gtest/gtest.h>

#include "db.hpp"
#include "elctl_fixture.hpp"

using elctl::testing::contains;
using elctl::testing::Outcome;
using elctl::testing::run_raw;

#ifndef NOBION_ELCTL_HAS_STORE

TEST(DbCmd, StubWithoutPersistence) {
  const Outcome o = run_raw({"db", "status", "--db", "sqlite::memory:"});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "elctl was built without persistence")) << o.err;
}

#else

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>

#include <QCoreApplication>
#include <QUrl>
#include <chrono>

#include "migrate.hpp"
#include "nobion/core/env.hpp"
#include "nobion/persistence/store.hpp"
#include "store_fixture.hpp"
#include "tiny/db.hpp"

namespace {

namespace ps = nobion::persistence;
namespace pd = nobion::persistence::detail;
namespace fs = std::filesystem;

class DbCmd : public elctl::testing::ElctlTest {
 protected:
  void SetUp() override {
    ElctlTest::SetUp();
    file_ = path("store.db");
    db_ = "sqlite:" + file_.string();
    auto store = ps::open_store(ps::StoreConfig{db_, true});
    ASSERT_TRUE(store) << to_string(store.error());
  }

  // The store as an older nobion left it: without the last migration.
  void take_back_the_last_migration() {
    {
      QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("db-cmd-test"));
      d.setDatabaseName(QString::fromStdString(file_.string()));
      ASSERT_TRUE(d.open());
      QSqlQuery q(d);
      for (const char* index : {"ref_object_position_ix", "ref_object_level_ix", "ref_object_irradiation_ix",
                                "ref_object_mass_spectrometer_ix"}) {
        ASSERT_TRUE(q.exec(QStringLiteral("DROP INDEX %1").arg(QString::fromLatin1(index))));
      }
      ASSERT_TRUE(q.exec(QStringLiteral("DELETE FROM schema_version WHERE version = 5")));
      d.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("db-cmd-test"));
  }

  fs::path file_;
  std::string db_;
};

}  // namespace

TEST_F(DbCmd, StatusOfACurrentStore) {
  const Outcome o = run_raw({"db", "status", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "up to date")) << o.out;
  EXPECT_TRUE(contains(o.out, "0005  ref_scope_indexes")) << o.out;
  EXPECT_TRUE(contains(o.out, "0001  init")) << o.out;
}

TEST_F(DbCmd, StatusOfAStoreThatIsBehindSaysWhatToRun) {
  take_back_the_last_migration();
  const Outcome o = run_raw({"db", "status", "--db", db_});
  EXPECT_EQ(o.code, elctl::kFailed);
  EXPECT_TRUE(contains(o.out, "behind")) << o.out;
  EXPECT_TRUE(contains(o.out, "migration 5")) << o.out;
  EXPECT_TRUE(contains(o.out, "elctl db migrate")) << o.out;
  // Asking changed nothing.
  EXPECT_FALSE(ps::open_store(ps::StoreConfig{db_, false}));
}

TEST_F(DbCmd, MigrateBringsAStoreUpToDate) {
  take_back_the_last_migration();
  ASSERT_FALSE(ps::open_store(ps::StoreConfig{db_, false}));

  const Outcome o = run_raw({"db", "migrate", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "migrated")) << o.out;
  EXPECT_TRUE(contains(o.out, "0005  ref_scope_indexes")) << o.out;
  EXPECT_TRUE(ps::open_store(ps::StoreConfig{db_, false}));  // what nobion does

  const Outcome again = run_raw({"db", "migrate", "--db", db_});
  EXPECT_EQ(again.code, elctl::kOk);
  EXPECT_TRUE(contains(again.out, "up to date")) << again.out;
  EXPECT_FALSE(contains(again.out, "migrated")) << again.out;
}

// PIs a former allowed-names list admitted were migrated by 0006 as persons
// with a family name only (PI spec 6.1): a migrate lists every such PI as a
// candidate to mark organisation, and changes none of them.
TEST_F(DbCmd, MigrateListsPisWithNoGivenName) {
  {
    auto store = ps::open_store(ps::StoreConfig{db_, true});
    ASSERT_TRUE(store) << to_string(store.error());
    const auto client = (*store)->register_client({"red-1", "reduction", std::nullopt, "test"});
    ASSERT_TRUE(client);
    ps::PrincipalInvestigatorSpec listed, ross, lab;
    listed.family_name = "NMGRL Lab";
    listed.affiliation = "NMT";
    ross.family_name = "Ross";
    ross.given_name = "J";
    lab.family_name = "Lab 7";
    lab.kind = ps::PiKind::Organisation;
    for (const auto& spec : {listed, ross, lab}) {
      ASSERT_TRUE((*store)->add_principal_investigator(*client, spec));
    }
  }
  take_back_the_last_migration();

  const Outcome o = run_raw({"db", "migrate", "--db", db_});
  ASSERT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "principal investigators with no given name")) << o.out;
  EXPECT_TRUE(contains(o.out, "  NMGRL Lab (NMT)\t")) << o.out;
  EXPECT_FALSE(contains(o.out, "Ross")) << o.out;
  EXPECT_FALSE(contains(o.out, "Lab 7")) << o.out;  // an organisation already

  auto store = ps::open_store(ps::StoreConfig{db_, false});
  ASSERT_TRUE(store);
  const auto pis = (*store)->principal_investigators();
  ASSERT_TRUE(pis);
  for (const auto& pi : *pis) {
    if (pi.family_name == "NMGRL Lab") {
      EXPECT_EQ(pi.kind, ps::PiKind::Person);  // nothing guessed
    }
  }

  // A store already up to date lists nothing: nothing was migrated.
  const Outcome again = run_raw({"db", "migrate", "--db", db_});
  EXPECT_FALSE(contains(again.out, "NMGRL Lab")) << again.out;
}

TEST_F(DbCmd, NeitherMakesADatabase) {
  const fs::path nowhere = path("typo.db");
  for (const char* action : {"status", "migrate"}) {
    const Outcome o = run_raw({"db", action, "--db", "sqlite:" + nowhere.string()});
    EXPECT_EQ(o.code, elctl::kUsage) << action;
    EXPECT_TRUE(contains(o.err, "no database at")) << o.err;
    EXPECT_FALSE(fs::exists(nowhere)) << action;
  }
}

TEST_F(DbCmd, AnEmptyFileIsNotAStore) {
  const fs::path empty = path("empty.db");
  { std::ofstream make(empty); }
  const Outcome o = run_raw({"db", "migrate", "--db", "sqlite:" + empty.string()});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "not a nobion store")) << o.err;
  EXPECT_EQ(fs::file_size(empty), 0u);
}

// No --db and no install to use: the usage error says why there is none.
TEST_F(DbCmd, WithoutDbTheReasonThereIsNoInstallIsSaid) {
  const Outcome none = run_raw({"db", "status"});
  EXPECT_EQ(none.code, elctl::kUsage);
  EXPECT_TRUE(contains(none.err, "nothing is installed yet")) << none.err;

  // Two installs, neither the default.
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("a").string(), "--name", "a", "--yes"}).code, elctl::kOk);
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", path("b").string(), "--name", "b", "--yes"}).code, elctl::kOk);
  {
    std::ifstream in(path("site.toml"));
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto at = text.find("default = ");
    ASSERT_NE(at, std::string::npos) << text;
    text.erase(at, text.find('\n', at) - at + 1);
    std::ofstream(path("site.toml"), std::ios::trunc) << text;
  }
  const Outcome several = run_raw({"db", "status"});
  EXPECT_EQ(several.code, elctl::kUsage);
  EXPECT_TRUE(contains(several.err, "several installs and no default")) << several.err;
}

TEST_F(DbCmd, UsageErrors) {
  EXPECT_EQ(run_raw({"db"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "status"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "vacuum", "--db", db_}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "status", "--db", db_, "--force"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "help"}).code, elctl::kOk);
}

// With --install and no --db: the install's database.
TEST_F(DbCmd, DbStatusUsesTheInstallsDatabase) {
  const fs::path root = path("dr");
  ASSERT_EQ(run_raw({"init", "data-reduction", "--root", root.string(), "--name", "dr", "--yes"}).code, elctl::kOk);
  const std::string url = "sqlite:" + (root / "data" / "nobion.db").generic_string();
  const Outcome by_url = run_raw({"db", "status", "--db", url});
  ASSERT_EQ(by_url.code, elctl::kOk) << by_url.err;
  const Outcome by_install = run_raw({"--install", "dr", "db", "status"});
  EXPECT_EQ(by_install.code, elctl::kOk) << by_install.err;
  EXPECT_EQ(by_install.out, by_url.out);
  // The default install, when --install is not given.
  const Outcome by_default = run_raw({"db", "status"});
  EXPECT_EQ(by_default.code, elctl::kOk) << by_default.err;
  EXPECT_EQ(by_default.out, by_url.out);
  // An install that is not there is an error, as for every command, not the default one.
  const Outcome nowhere = run_raw({"--install", "nowhere", "db", "status"});
  EXPECT_EQ(nowhere.code, elctl::kFailed);
  EXPECT_FALSE(contains(nowhere.out, "up to date")) << nowhere.out;
}

// Field diagnostics spec, section 11: a SQLite store is copied before a
// migration changes it, and the copy is named on the way out.
class ElctlDb : public DbCmd {};

TEST_F(ElctlDb, MigratePrintsTheBackup) {
  take_back_the_last_migration();
  const Outcome o = run_raw({"db", "migrate", "--db", db_});
  ASSERT_EQ(o.code, elctl::kOk) << o.err;
  const auto at = o.out.find("backed up: ");
  ASSERT_NE(at, std::string::npos) << o.out;
  const std::string shown = o.out.substr(at + 11, o.out.find('\n', at) - at - 11);
  const fs::path backup(shown);
  EXPECT_TRUE(fs::equivalent(backup.parent_path(), file_.parent_path())) << shown;  // links resolved
  EXPECT_TRUE(backup.filename().string().starts_with("store.pre-")) << shown;
  EXPECT_TRUE(fs::is_regular_file(backup)) << shown;
  // The copy is the store as it was: still behind.
  EXPECT_FALSE(ps::open_store(ps::StoreConfig{"sqlite:" + shown, false}));

  // Nothing pending, nothing copied.
  const Outcome again = run_raw({"db", "migrate", "--db", db_});
  EXPECT_EQ(again.code, elctl::kOk) << again.err;
  EXPECT_FALSE(contains(again.out, "backed up")) << again.out;
}

// nobion does not copy a PostgreSQL store: elctl db migrate refuses one with
// migrations pending until told it is backed up, and says how.
TEST_F(ElctlDb, PostgresMigrateNeedsBackedUp) {
  const auto base_env = nobion::env_var("NOBION_TEST_PG_URL");
  if (!base_env || base_env->empty()) GTEST_SKIP() << "NOBION_TEST_PG_URL not set";
  // A password in the URL even when the server asks for none, so that the
  // test can see it is not printed.
  QUrl base(QString::fromStdString(*base_env));
  if (base.password().isEmpty()) base.setPassword(QStringLiteral("not-printed-pw"));
  const std::string password = base.password().toStdString();
  const std::string schema = "nobion_t_elctl_" + std::to_string(QCoreApplication::applicationPid()) + "_" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string base_url = base.toString(QUrl::FullyEncoded).toStdString();
  const std::string url =
      base_url + (base_url.find('?') == std::string::npos ? "?" : "&") + "search_path=" + schema;
  auto admin = pd::Db::open(ps::StoreConfig{base_url, false});
  ASSERT_TRUE(admin) << to_string(admin.error());
  ASSERT_TRUE((*admin)->unprepared(QString::fromStdString("CREATE SCHEMA " + schema)));
  struct Drop {
    pd::Db& db;
    std::string schema;
    ~Drop() { (void)db.unprepared(QString::fromStdString("DROP SCHEMA IF EXISTS " + schema + " CASCADE")); }
  } drop{**admin, schema};
  int latest = 0;
  {
    auto db = pd::Db::open(ps::StoreConfig{url, false});
    ASSERT_TRUE(db) << to_string(db.error());
    auto memory = ps::open_store(ps::StoreConfig{"sqlite::memory:", true});
    ASSERT_TRUE(memory);
    auto status = (*memory)->schema_status();
    ASSERT_TRUE(status && !status->empty());
    latest = status->back().version;
    auto behind = pd::migrate(**db, true, latest - 1);
    ASSERT_TRUE(behind) << to_string(behind.error());
  }

  const Outcome refused = run_raw({"db", "migrate", "--db", url});
  EXPECT_NE(refused.code, elctl::kOk);
  EXPECT_TRUE(contains(refused.out, "pg_dump --format=custom")) << refused.out;
  EXPECT_TRUE(contains(refused.out, "--backed-up")) << refused.out;
  // search_path is nobion's, not libpq's: pg_dump is given the schema instead.
  const auto dump_at = refused.out.find("pg_dump");
  const std::string dump =
      dump_at == std::string::npos ? std::string() : refused.out.substr(dump_at, refused.out.find('\n', dump_at) - dump_at);
  EXPECT_TRUE(contains(dump, "--schema=" + schema)) << dump;
  EXPECT_FALSE(contains(dump, "search_path")) << dump;
  EXPECT_FALSE(contains(refused.out, password)) << refused.out;
  EXPECT_FALSE(contains(refused.err, password)) << refused.err;
  EXPECT_FALSE(ps::open_store(ps::StoreConfig{url, false}));  // nothing migrated

  const Outcome migrated = run_raw({"db", "migrate", "--db", url, "--backed-up"});
  EXPECT_EQ(migrated.code, elctl::kOk) << migrated.err;
  EXPECT_TRUE(contains(migrated.out, "migrated")) << migrated.out;
  EXPECT_FALSE(contains(migrated.out, "backed up: ")) << migrated.out;
  EXPECT_TRUE(ps::open_store(ps::StoreConfig{url, false}));
}

// A fresh PostgreSQL database has nothing to lose: no flag needed.
TEST_F(ElctlDb, AFreshPostgresStoreNeedsNoBackup) {
  const auto base_env = nobion::env_var("NOBION_TEST_PG_URL");
  if (!base_env || base_env->empty()) GTEST_SKIP() << "NOBION_TEST_PG_URL not set";
  const std::string schema = "nobion_t_elctl_" + std::to_string(QCoreApplication::applicationPid()) + "_" +
                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string url =
      *base_env + (base_env->find('?') == std::string::npos ? "?" : "&") + "search_path=" + schema;
  auto admin = pd::Db::open(ps::StoreConfig{*base_env, false});
  ASSERT_TRUE(admin) << to_string(admin.error());
  ASSERT_TRUE((*admin)->unprepared(QString::fromStdString("CREATE SCHEMA " + schema)));
  struct Drop {
    pd::Db& db;
    std::string schema;
    ~Drop() { (void)db.unprepared(QString::fromStdString("DROP SCHEMA IF EXISTS " + schema + " CASCADE")); }
  } drop{**admin, schema};
  const Outcome o = run_raw({"db", "migrate", "--db", url});
  EXPECT_EQ(o.code, elctl::kOk) << o.err << o.out;
}

// The printed command holds no password: not one with '/' and '@' in it
// (percent-encoded, as a URL carries them), not a password= query item. The
// schema argument is quoted whole, so a search_path with a space or a '$'
// pastes as one argument and runs nothing.
TEST(ElctlDbPrint, ThePgDumpCommandHoldsNoPassword) {
  const std::string url =
      "postgresql://lab:s%2Fe%40cr3t@db.example.org:5433/nobion?sslmode=require&password=qpass&search_path=my%20schema%24x";
  const std::string command = elctl::pg_dump_command(url);
  for (const char* secret : {"s%2Fe%40cr3t", "s/e@cr3t", "cr3t", "qpass", "password"})
    EXPECT_FALSE(contains(command, secret)) << secret << ": " << command;
  EXPECT_EQ(command,
            "pg_dump --format=custom --file=nobion-pre-migrate.dump '--schema=my schema$x' "
            "'postgresql://lab@db.example.org:5433/nobion?sslmode=require'");
  const std::string shown = elctl::printable_store_url(url);
  for (const char* secret : {"cr3t", "qpass", "password"}) EXPECT_FALSE(contains(shown, secret)) << shown;
  EXPECT_TRUE(contains(shown, "search_path=")) << shown;
  // A quote in the schema closes and reopens the quoting.
  EXPECT_TRUE(contains(elctl::pg_dump_command("postgresql://u@h/d?search_path=o'k"), "'--schema=o'\\''k'"))
      << elctl::pg_dump_command("postgresql://u@h/d?search_path=o'k");
}

TEST_F(ElctlDb, BackedUpIsOnlyMigrates) {
  EXPECT_EQ(run_raw({"db", "status", "--db", db_, "--backed-up"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "migrate", "--db", db_, "--backed-up"}).code, elctl::kOk);
}

// elctl db derive, on the seeded store: two heating steps of an irradiated
// unknown, which have ages, and an air, which has none.
class DbDerive : public elctl::testing::StoreTest {};

TEST_F(DbDerive, FillsTheCacheAndSaysSo) {
  const Outcome o = run_raw({"db", "derive", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "  3 seen, 3 written")) << o.out;  // the page's line
  EXPECT_TRUE(contains(o.out, "derived: 3 seen, 3 written (1 without an age), 0 current, 0 failed")) << o.out;
  EXPECT_TRUE(o.err.empty()) << o.err;

  const Outcome again = run_raw({"db", "derive", "--db", db_});
  EXPECT_EQ(again.code, elctl::kOk) << again.err;
  EXPECT_TRUE(contains(again.out, "derived: 3 seen, 0 written (0 without an age), 3 current, 0 failed")) << again.out;
}

TEST_F(DbDerive, AllWritesAgainAndQuietSaysOnlyTheSummary) {
  ASSERT_EQ(run_raw({"db", "derive", "--db", db_}).code, elctl::kOk);
  const Outcome o = run_raw({"db", "derive", "--db", db_, "--all", "--quiet"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_EQ(o.out, "derived: 3 seen, 3 written (1 without an age), 0 current, 0 failed\n");
}

TEST_F(DbDerive, TheLimitIsHowManyAreWalked) {
  const Outcome o = run_raw({"db", "derive", "--db", db_, "--limit", "1", "--quiet"});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_TRUE(contains(o.out, "derived: 1 seen")) << o.out;
  for (const char* bad : {"0", "-2", "x", "1.5"})
    EXPECT_EQ(run_raw({"db", "derive", "--db", db_, "--limit", bad}).code, elctl::kUsage) << bad;
}

// A domain with nothing to derive (noble_gas) writes no cache of ages.
TEST_F(DbDerive, ANobleGasInstallDerivesNothing) {
  const Outcome o = run_raw({"--domain", "noble_gas", "db", "derive", "--db", db_});
  EXPECT_EQ(o.code, elctl::kOk) << o.err;
  EXPECT_EQ(o.out, "derive: nothing to derive under domain noble_gas\n");
  EXPECT_TRUE(o.err.empty()) << o.err;
  store_.reset();
  qint64 rows = -1;
  {
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("db-derive-none-test"));
    d.setDatabaseName(QString::fromStdString(path("store.db").string()));
    ASSERT_TRUE(d.open());
    QSqlQuery q(d);
    ASSERT_TRUE(q.exec(QStringLiteral("SELECT count(*) FROM derived_value")));
    ASSERT_TRUE(q.next());
    rows = q.value(0).toLongLong();
    d.close();
  }
  QSqlDatabase::removeDatabase(QStringLiteral("db-derive-none-test"));
  EXPECT_EQ(rows, 0);
}

TEST_F(DbDerive, AStoreBehindItsMigrationsIsRefused) {
  store_.reset();
  {
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("db-derive-test"));
    d.setDatabaseName(QString::fromStdString(path("store.db").string()));
    ASSERT_TRUE(d.open());
    QSqlQuery q(d);
    ASSERT_TRUE(q.exec(QStringLiteral("DELETE FROM schema_version WHERE version = 5")));
    d.close();
  }
  QSqlDatabase::removeDatabase(QStringLiteral("db-derive-test"));
  const Outcome o = run_raw({"db", "derive", "--db", db_});
  EXPECT_EQ(o.code, elctl::kUsage);
  EXPECT_TRUE(contains(o.err, "migration")) << o.err;
  EXPECT_FALSE(ps::open_store(ps::StoreConfig{db_, false}));  // still behind: derive never migrates
}

TEST_F(DbDerive, ItsOptionsAreOnlyDerives) {
  EXPECT_EQ(run_raw({"db", "status", "--db", db_, "--all"}).code, elctl::kUsage);
  EXPECT_EQ(run_raw({"db", "migrate", "--db", db_, "--limit", "3"}).code, elctl::kUsage);
}

#endif
