#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
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
  EXPECT_EQ(redact::in_text("amqp://u:p'w@h"), "amqp://u:***@h");
  EXPECT_EQ(redact::in_text("url = \"pg://u:p\\\"w@h/db\""), "url = \"pg://u:***@h/db\"");
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

namespace {

// Runs `fn` and fails when it takes 5 s or more (an upper bound only; the
// real cost is far below it). Prints the time.
template <class Fn>
std::string timed(const char* what, Fn&& fn) {
  const auto begin = std::chrono::steady_clock::now();
  std::string out = fn();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin);
  std::cout << "[ timing ] " << what << ": " << ms.count() << " ms\n";
  EXPECT_LT(ms.count(), 5000) << what;
  return out;
}

std::string repeat(std::string_view unit, std::size_t bytes) {
  std::string out;
  out.reserve(bytes + unit.size());
  while (out.size() < bytes) out += unit;
  return out;
}

}  // namespace

TEST(Redact, TomlDottedKeysWithQuotedPartsAndSpaces) {
  EXPECT_EQ(redact::in_toml("a.\"password\" = \"x\""), "a.\"password\" = \"***\"");
  EXPECT_EQ(redact::in_toml("\"a\".password = \"x\""), "\"a\".password = \"***\"");
  EXPECT_EQ(redact::in_toml("a . password = \"x\""), "a . password = \"***\"");
  EXPECT_EQ(redact::in_toml("x.'token' = 'x'"), "x.'token' = \"***\"");
  EXPECT_EQ(redact::in_toml("x.\"keyboard\" = 'x'"), "x.\"keyboard\" = 'x'");
}

TEST(Redact, TomlNestingIsCappedAndNeverOverflows) {
  const std::string deep = repeat("[", 100000) + repeat("]", 100000);
  const std::string secret = timed("deep secret", [&] { return redact::in_toml("password = " + deep); });
  EXPECT_EQ(secret, "password = \"***\"");
  const std::string other = timed("deep other", [&] { return redact::in_toml("x = " + deep + "\nurl = \"pg://u:pw@h\""); });
  EXPECT_EQ(other, "x = " + deep + "\nurl = \"pg://u:***@h\"");
  const std::string tables = timed("deep tables", [&] {
    return redact::in_toml("x = " + repeat("{a=", 100000) + "1" + repeat("}", 100000));
  });
  EXPECT_FALSE(tables.empty());
}

TEST(Redact, LinearOnTenMegabytes) {
  const std::size_t ten_mb = 10U * 1024U * 1024U;
  const std::string scheme_chain = repeat("a://", ten_mb);
  EXPECT_EQ(timed("in_text a://a://", [&] { return redact::in_text(scheme_chain); }).size(), scheme_chain.size());
  const std::string quoted_urls = repeat("\"http://a\",", ten_mb);
  EXPECT_EQ(timed("in_text quoted urls", [&] { return redact::in_text(quoted_urls); }).size(), quoted_urls.size());
  const std::string log = repeat("2026-11-06 12:00:00 INFO link up host=h port=5 note=\"ok\"\n", ten_mb);
  EXPECT_EQ(timed("in_text log", [&] { return redact::in_text(log); }), log);
  EXPECT_EQ(timed("in_toml log", [&] { return redact::in_toml(log); }), log);
  const std::string openers = repeat("x = [\n", ten_mb);
  EXPECT_EQ(timed("in_toml unterminated arrays", [&] { return redact::in_toml(openers); }), openers);
  const std::string triples = repeat("x = \"\"\"\n", ten_mb);
  EXPECT_EQ(timed("in_toml unterminated strings", [&] { return redact::in_toml(triples); }), triples);
  const std::string run = "x = {" + repeat("a", ten_mb) + "}";
  EXPECT_EQ(timed("in_toml long run in table", [&] { return redact::in_toml(run); }), run);
}

TEST(Redact, ByteOrderMarkBeforeTheFirstKey) {
  EXPECT_EQ(redact::in_toml("\xEF\xBB\xBFpassword = \"x\"\nhost = \"h\"\n"), "\xEF\xBB\xBFpassword = \"***\"\nhost = \"h\"\n");
}

TEST(Redact, UnterminatedValuesEndAtTheirFirstLine) {
  EXPECT_EQ(redact::in_toml("x = [ 1,\npassword = \"p\""), "x = [ 1,\npassword = \"***\"");
  EXPECT_EQ(redact::in_toml("x = \"\"\"abc\npassword = \"p\"\n"), "x = \"\"\"abc\npassword = \"***\"\n");
  EXPECT_EQ(redact::in_toml("token = [abc\nhost = \"h\"\nmore = 1\n"), "token = \"***\"\nhost = \"h\"\nmore = 1\n");
  EXPECT_EQ(redact::in_toml("secret = \"\"\"abc\nhost = \"h\"\n"), "secret = \"***\"\nhost = \"h\"\n");
}

TEST(Redact, BareSecretValueRunsToEndOfLine) {
  EXPECT_EQ(redact::in_toml("password = ab,cd\n"), "password = \"***\"\n");
  EXPECT_EQ(redact::in_toml("secret = ab]cd # note\nx = 1\n"), "secret = \"***\" # note\nx = 1\n");
  EXPECT_EQ(redact::in_toml("password = ab#cd\n"), "password = \"***\"\n");
  EXPECT_EQ(redact::in_toml("t = { password = ab, x = 1 }"), "t = { password = \"***\", x = 1 }");
}

TEST(Redact, ValueOnTheNextLineIsMasked) {
  EXPECT_EQ(redact::in_toml("password =\nhunter2\nhost = \"h\"\n"), "password =\n***\nhost = \"h\"\n");
  EXPECT_EQ(redact::in_toml("password =\r\n\r\nhunter2\r\nx = 1"), "password =\r\n\r\n***\r\nx = 1");
  EXPECT_EQ(redact::in_toml("password ="), "password =");
}
