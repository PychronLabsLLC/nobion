#include <gtest/gtest.h>

#include <string>

#include <toml++/toml.hpp>

#include "nobion/core/redact.hpp"

using namespace nobion;

TEST(Redact, KeysAreSecretEqualOrBySuffix) {
  for (const char* key : {"password", "DB_Password", "api_key", "smtp-passwd", "credential", "brevo_key", "Token",
                          "client_secret", "apiKey", "dbPassword", "tokens", "passwords", "Secrets", "api_keys"}) {
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

TEST(Redact, UrlPasswordsWithOddCharacters) {
  EXPECT_EQ(redact::in_text("http://user:p/w@host/x"), "http://user:***@host/x");
  EXPECT_EQ(redact::in_text("http://user:p?w@host/x"), "http://user:***@host/x");
  EXPECT_EQ(redact::in_text("http://user:p#w@host/x"), "http://user:***@host/x");
  EXPECT_EQ(redact::in_text("x://u:p@w@h/db"), "x://u:***@h/db");
  EXPECT_EQ(redact::in_text("HTTPS://u:pw@h"), "HTTPS://u:***@h");
  EXPECT_EQ(redact::in_text("https://u@h:80/x"), "https://u@h:80/x");
  const std::string nested = redact::in_text("a://b://u:pw@h");
  EXPECT_EQ(nested.find("pw"), std::string::npos) << nested;
}

TEST(Redact, TomlQuotingAndSpacing) {
  EXPECT_EQ(redact::in_toml("password='x'"), "password=\"***\"");
  EXPECT_EQ(redact::in_toml("password\t=\t\"x\""), "password\t=\t\"***\"");
  EXPECT_EQ(redact::in_toml("db.password = \"x\""), "db.password = \"***\"");
  EXPECT_EQ(redact::in_toml("\"db.password\" = \"x\""), "\"db.password\" = \"***\"");
  EXPECT_EQ(redact::in_toml("\"Admin Password\" = \"x\""), "\"Admin Password\" = \"***\"");
  EXPECT_EQ(redact::in_toml("password = \"\""), "password = \"***\"");
  EXPECT_EQ(redact::in_toml("password = \"a#b=c\\\"d\" # c"), "password = \"***\" # c");
  EXPECT_EQ(redact::in_toml("[password]\nhost = \"h\"\n"), "[password]\nhost = \"h\"\n");
  EXPECT_EQ(redact::in_toml("apiKey = \"x\"\r\ntoken = 123\r\nname = \"n\"\r\n"),
            "apiKey = \"***\"\r\ntoken = \"***\"\r\nname = \"n\"\r\n");
  EXPECT_EQ(redact::in_toml("db = { url = \"pg://u:pw@h/d\", keyboard = 1 }"),
            "db = { url = \"pg://u:***@h/d\", keyboard = 1 }");
}

TEST(Redact, TomlMultiLineValuesUnderSecretKeys) {
  const std::string text =
      "a = 1\npassword = \"\"\"\nhunter2\n\"\"\"\ntoken = [\n\"s1\",\n\"s2\"\n]\n"
      "secret = '''\nzzz\n'''  # c\nb = 2\ncredential = {\na = 1\n}\n";
  const std::string out = redact::in_toml(text);
  EXPECT_EQ(out, "a = 1\npassword = \"***\"\ntoken = \"***\"\nsecret = \"***\"  # c\nb = 2\ncredential = \"***\"\n");
  const auto parsed = toml::parse(out);
  EXPECT_TRUE(static_cast<bool>(parsed)) << out;
}

TEST(Redact, TomlMultiLineValuesUnderOtherKeysPassThroughButUrlsAreMasked) {
  const std::string text =
      "list = [\n  \"a\",\n  \"pg://u:pw@h/d\", # password = \"x\"\n]\ntext = \"\"\"\n[not] a header\n\"\"\"\n";
  EXPECT_EQ(redact::in_toml(text),
            "list = [\n  \"a\",\n  \"pg://u:***@h/d\", # password = \"x\"\n]\ntext = \"\"\"\n[not] a header\n\"\"\"\n");
}

TEST(Redact, TomlTablesNestedInArraysAndTables) {
  EXPECT_EQ(redact::in_toml("x = [ { password = \"x\" } ]"), "x = [ { password = \"***\" } ]");
  EXPECT_EQ(redact::in_toml("users = [{ name = \"a\", token = \"t\" }, { name = \"b\", keyboard = 1 }]"),
            "users = [{ name = \"a\", token = \"***\" }, { name = \"b\", keyboard = 1 }]");
  EXPECT_EQ(redact::in_toml("a = { b = { password = \"x\", c = [ { key = 1 } ] } }"),
            "a = { b = { password = \"***\", c = [ { key = \"***\" } ] } }");
}
