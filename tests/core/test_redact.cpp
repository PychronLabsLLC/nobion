#include <gtest/gtest.h>

#include <string>

#include "nobion/core/redact.hpp"

using namespace nobion;

TEST(Redact, KeysAreSecretEqualOrBySuffix) {
  for (const char* key : {"password", "DB_Password", "api_key", "smtp-passwd", "credential", "brevo_key", "Token",
                          "client_secret"}) {
    EXPECT_TRUE(redact::is_secret_key(key)) << key;
  }
  for (const char* key : {"keyboard", "monkey", "tokenizer", "", "_key_x", "name"}) {
    EXPECT_FALSE(redact::is_secret_key(key)) << key;
  }
}

TEST(Redact, UrlPasswordsAreMasked) {
  EXPECT_EQ(redact::in_text("postgres://u:pw@h/db"), "postgres://u:***@h/db");
  EXPECT_EQ(redact::in_text("https://h/x"), "https://h/x");
  EXPECT_EQ(redact::in_text("https://u@h/x"), "https://u@h/x");
  EXPECT_EQ(redact::in_text("a postgres://u:pw@h:5432/db and https://x:y@z?q=1 end"),
            "a postgres://u:***@h:5432/db and https://x:***@z?q=1 end");
  EXPECT_EQ(redact::in_text("no url here"), "no url here");
}

TEST(Redact, TomlByKeyAndByValue) {
  EXPECT_EQ(redact::in_toml("password = \"x\""), "password = \"***\"");
  EXPECT_EQ(redact::in_toml("database = \"postgres://u:pw@h/db\""), "database = \"postgres://u:***@h/db\"");
  EXPECT_EQ(redact::in_toml("# password = \"x\"\n[db]\nhost = \"h\"\nkeyboard = \"us\"\n"),
            "# password = \"x\"\n[db]\nhost = \"h\"\nkeyboard = \"us\"\n");
  EXPECT_EQ(redact::in_toml("api_key = \"abc\" # note\nport = 5"), "api_key = \"***\" # note\nport = 5");
  EXPECT_EQ(redact::in_toml("token = 12345"), "token = \"***\"");
}

TEST(Redact, TomlInlineTables) {
  EXPECT_EQ(redact::in_toml("db = { user = \"u\", password = \"x\" }"), "db = { user = \"u\", password = \"***\" }");
  EXPECT_EQ(redact::in_toml("db = { monkey = \"m\", tokenizer = \"t\", api_key = \"k\" } # c"),
            "db = { monkey = \"m\", tokenizer = \"t\", api_key = \"***\" } # c");
}
