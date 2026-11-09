#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "nobion/core/error.hpp"
#include "nobion/persistence/store.hpp"
#include "tiny/db.hpp"

namespace nobion::persistence::detail {

struct BackupPolicy {
  bool enabled = false;
  std::function<void(const std::filesystem::path&)> on_backup;
};

// The code of the Config error a store behind its migrations gives with
// apply = false (an empty store's has none).
inline constexpr const char* kCodeSchemaBehind = "schema_behind";

// Generated at configure time from migrations/<dialect>/NNNN_<name>.sql.
struct EmbeddedMigration {
  const char* dialect;  // "pg" | "sqlite"
  const char* file;     // "0001_init.sql"
  const unsigned char* data;
  std::size_t size;
};
extern const EmbeddedMigration kEmbeddedMigrations[];
extern const std::size_t kEmbeddedMigrationCount;

// Splits a migration into statements: ';' outside quotes, -- comments and
// $$ bodies ends a statement, except inside a SQLite trigger body
// (CREATE TRIGGER ... BEGIN ... END;).
std::vector<std::string> split_sql(std::string_view sql);

// Applies pending migrations in order, one transaction each, recording
// version, description and SHA-256 checksum in schema_version (section 11.4).
// A recorded checksum that differs from the embedded file is a Config error.
// With apply = false, any pending migration is a Config error instead.
// up_to stops after that version (a test that needs a store as an older build
// left it); unset, every migration this build carries.
//
// With apply and policy.enabled, a store that has migrations applied and lacks
// one is copied first, outside any transaction (field diagnostics spec,
// section 11): a SQLite file store with VACUUM INTO beside itself; an
// in-memory store and PostgreSQL are not copied. A copy that cannot be made
// refuses the migration (Io). policy.on_backup is called with the copy's path.
Result<std::vector<AppliedMigration>> migrate(Db& db, bool apply, std::optional<int> up_to = std::nullopt,
                                              const BackupPolicy& policy = {});

}  // namespace nobion::persistence::detail
