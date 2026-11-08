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

// Under AddressSanitizer every case costs several times as much (CI's Linux
// ASan job took 6.8 s for one 10 MB case): there the input is 2 MB, still
// hours for anything quadratic, and the 5 s bound holds.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#elif defined(__has_feature) && !defined(__CPPCHECK__)
#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizer = true;
#else
constexpr bool kAddressSanitizer = false;
#endif
#else
constexpr bool kAddressSanitizer = false;
#endif

TEST(Redact, LinearOnTenMegabytes) {
  const std::size_t ten_mb = (kAddressSanitizer ? 2U : 10U) * 1024U * 1024U;
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
  const std::string dotted = "x = {" + repeat("a.", ten_mb) + "}";
  EXPECT_EQ(timed("in_toml dotted run in table", [&] { return redact::in_toml(dotted); }), dotted);
  const std::string queries = repeat("?token=&x=1&", ten_mb);
  EXPECT_EQ(timed("in_text query parameters", [&] { return redact::in_text(queries); }).size(), queries.size());
  const std::string names = "?" + repeat("a", ten_mb);
  EXPECT_EQ(timed("in_text long parameter name", [&] { return redact::in_text(names); }), names);
  const std::string hooks = repeat("[[webhook]]\nurl = \"https://h/x\"\n", ten_mb);
  EXPECT_EQ(timed("in_toml webhook tables", [&] { return redact::in_toml(hooks); }).find("https://h/x"),
            std::string::npos);
  const std::string spaced = "x = {" + repeat("a . ", ten_mb) + "}";
  EXPECT_EQ(timed("in_toml spaced dotted run in table", [&] { return redact::in_toml(spaced); }), spaced);
}

TEST(Redact, ADottedRunWithNoKeyIsSkippedAndTheNextKeyIsStillRead) {
  EXPECT_EQ(redact::in_toml("x = { a.b., password = \"p\" }"), "x = { a.b., password = \"***\" }");
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

// A webhook's URL is its own credential (a Slack hook path is a bearer
// secret): its value is masked whole under a hook table or key.
TEST(Redact, WebhookUrlsAreMaskedWhole) {
  const std::string slack = "https://hooks.slack.com/services/T000/B000/XXXX";
  EXPECT_EQ(redact::in_toml("[[webhook]]\nname = \"lab\"\nurl = \"" + slack + "\"\n"),
            "[[webhook]]\nname = \"lab\"\nurl = \"***\"\n");
  EXPECT_EQ(redact::in_toml("[webhook]\nURL = \"" + slack + "\"\n"), "[webhook]\nURL = \"***\"\n");
  EXPECT_EQ(redact::in_toml("[notify.hooks]\nurl = 'x'\n"), "[notify.hooks]\nurl = \"***\"\n");
  EXPECT_EQ(redact::in_toml("webhook_url = \"" + slack + "\"\n"), "webhook_url = \"***\"\n");
  EXPECT_EQ(redact::in_toml("slack_hook = \"" + slack + "\"\n"), "slack_hook = \"***\"\n");
  EXPECT_EQ(redact::in_toml("webhook.url = \"" + slack + "\"\n"), "webhook.url = \"***\"\n");
  EXPECT_EQ(redact::in_toml("[[webhook]]\nopts = { url = \"x\", n = 1 }\n"),
            "[[webhook]]\nopts = { url = \"***\", n = 1 }\n");
  // Outside a hook table a url keeps its value (its password still masked).
  EXPECT_EQ(redact::in_toml("[[webhook]]\nurl = \"a\"\n[database]\nurl = \"postgres://u:pw@h/db\"\n"),
            "[[webhook]]\nurl = \"***\"\n[database]\nurl = \"postgres://u:***@h/db\"\n");
  EXPECT_EQ(redact::in_toml("url = \"https://h/x\"\n"), "url = \"https://h/x\"\n");
}

// The notifications.toml example, as a lab fills it in.
TEST(Redact, NotificationsWebhookShape) {
  const std::string toml =
      "[[webhook]]\n"
      "name = \"slack\"\n"
      "url = \"https://hooks.slack.com/services/T000/B000/XXXX\"  # the hook\n"
      "events = [\"run_failed\"]\n";
  const std::string masked = redact::in_toml(toml);
  EXPECT_EQ(masked.find("hooks.slack.com"), std::string::npos) << masked;
  EXPECT_NE(masked.find("# the hook"), std::string::npos) << masked;
  EXPECT_NE(masked.find("run_failed"), std::string::npos) << masked;
}

// A query parameter named as a secret keeps its name and loses its value.
TEST(Redact, SecretQueryParametersAreMasked) {
  EXPECT_EQ(redact::in_text("?token=abc&x=1"), "?token=***&x=1");
  EXPECT_EQ(redact::in_text("GET https://h/p?a=1&api_key=k3y#frag done"), "GET https://h/p?a=1&api_key=***#frag done");
  EXPECT_EQ(redact::in_text("https://u:pw@h/p?Token=t"), "https://u:***@h/p?Token=***");
  EXPECT_EQ(redact::in_text("\"https://h/?secret=s\""), "\"https://h/?secret=***\"");
  EXPECT_EQ(redact::in_text("?keyboard=1&monkey=2&token="), "?keyboard=1&monkey=2&token=");
  EXPECT_EQ(redact::in_text("a & b=c ?d"), "a & b=c ?d");
}
