#include "nobion/core/redact.hpp"

#include <array>
#include <cctype>
#include <cstddef>

namespace nobion::redact {
namespace {

constexpr std::size_t kNone = std::string_view::npos;

bool equals_nocase(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

bool contains_nocase(std::string_view text, std::string_view word) noexcept {
  if (word.size() > text.size()) return false;
  for (std::size_t i = 0; i + word.size() <= text.size(); ++i) {
    if (equals_nocase(text.substr(i, word.size()), word)) return true;
  }
  return false;
}

bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }
bool is_line_break(char c) noexcept { return c == '\n' || c == '\r'; }
bool is_upper(char c) noexcept { return std::isupper(static_cast<unsigned char>(c)) != 0; }
bool is_lower(char c) noexcept { return std::islower(static_cast<unsigned char>(c)) != 0; }

bool is_bare_key_char(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-';
}

bool ends_with_word(std::string_view key, std::string_view name) noexcept {
  if (equals_nocase(key, name)) return true;
  if (key.size() <= name.size()) return false;
  const std::string_view tail = key.substr(key.size() - name.size());
  if (!equals_nocase(tail, name)) return false;
  const char before = key[key.size() - name.size() - 1];
  if (before == '_' || before == '-') return true;
  return is_upper(tail.front()) && is_lower(before);  // camelCase: apiKey
}

// The part of a key after its last '.' or space: "db.password" is judged as "password".
std::string_view last_part(std::string_view key) noexcept {
  const std::size_t cut = key.find_last_of(". ");
  return cut == kNone ? key : key.substr(cut + 1);
}

// One past the end of the quoted string starting at s[i]; a string that is not
// closed on its line ends there.
std::size_t quoted_end(std::string_view s, std::size_t i) noexcept {
  const char q = s[i];
  for (std::size_t j = i + 1; j < s.size(); ++j) {
    if (q == '"' && s[j] == '\\') {
      ++j;
    } else if (s[j] == q) {
      return j + 1;
    } else if (s[j] == '\n') {
      return j;
    }
  }
  return s.size();
}

bool is_triple(std::string_view s, std::size_t i) noexcept {
  return i + 2 < s.size() && s[i + 1] == s[i] && s[i + 2] == s[i];
}

// One past the closing triple quote of the string starting at s[i] (three
// double or three single quotes), or kNone when the text ends first.
std::size_t triple_end(std::string_view s, std::size_t i) noexcept {
  const char q = s[i];
  for (std::size_t j = i + 3; j < s.size(); ++j) {
    if (q == '"' && s[j] == '\\') {
      ++j;
    } else if (s[j] == q && is_triple(s, j)) {
      j += 3;
      while (j < s.size() && s[j] == q) ++j;  // up to two quotes may belong to the content
      return j;
    }
  }
  return kNone;
}

// Past this many levels an array or table is copied, not read: no recursion
// deep enough to overflow, and a cost that does not grow with depth.
constexpr int kMaxDepth = 64;

// Work budget for scans that ran to the end of the text without finding a
// close (an unterminated value): once spent, no multi-line value is searched
// for, so such scans cost a bounded multiple of the input.
struct Scan {
  std::size_t wasted = 0;
  std::size_t limit = 0;
  // The current [table] or [[array of tables]] header names a hook.
  bool hook_table = false;
};

// A hook's URL is its credential (a Slack webhook path is a bearer token):
// a key holding "hook" anywhere, or `url` under a hook table or dotted hook
// key (`[[webhook]] url`, `webhook.url`).
bool is_hook_secret(const Scan& scan, std::string_view whole_key, std::string_view last) noexcept {
  if (contains_nocase(last, "hook")) return true;
  return equals_nocase(last, "url") && (scan.hook_table || contains_nocase(whole_key, "hook"));
}

// The end of the line holding s[i] (before its '\r' or '\n').
std::size_t line_end(std::string_view s, std::size_t i) noexcept {
  while (i < s.size() && !is_line_break(s[i])) ++i;
  return i;
}

// One past the end of the TOML value starting at s[i]: a string (multi-line
// too), an array or inline table (across lines), or a bare value. kNone: a
// multi-line value with no close before the end of the text.
std::size_t value_end(Scan& scan, std::string_view s, std::size_t i) noexcept {
  if (i >= s.size()) return s.size();
  const char c = s[i];
  if (c == '"' || c == '\'') {
    if (!is_triple(s, i)) return quoted_end(s, i);
    if (scan.wasted > scan.limit) return kNone;
    const std::size_t end = triple_end(s, i);
    if (end == kNone) scan.wasted += s.size() - i;
    return end;
  }
  if (c == '{' || c == '[') {
    if (scan.wasted > scan.limit) return kNone;
    int depth = 0;
    for (std::size_t j = i; j < s.size(); ++j) {
      const char d = s[j];
      if (d == '"' || d == '\'') {
        const std::size_t end = is_triple(s, j) ? triple_end(s, j) : quoted_end(s, j);
        if (end == kNone) break;
        j = end - 1;
      } else if (d == '#') {
        while (j + 1 < s.size() && s[j + 1] != '\n') ++j;
      } else if (d == '{' || d == '[') {
        ++depth;
      } else if (d == '}' || d == ']') {
        if (--depth == 0) return j + 1;
      }
    }
    scan.wasted += s.size() - i;
    return kNone;
  }
  std::size_t j = i;
  while (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' && s[j] != '#' && !is_line_break(s[j])) ++j;
  while (j > i && is_space(s[j - 1])) --j;
  return j;
}

// The end of a bare value under a secret key at the top level: the line, less
// a trailing ` # comment`.
std::size_t bare_secret_end(std::string_view s, std::size_t i) noexcept {
  const std::size_t eol = line_end(s, i);
  std::size_t end = eol;
  for (std::size_t j = i + 1; j < eol; ++j) {
    if (s[j] == '#' && is_space(s[j - 1])) {
      end = j;
      break;
    }
  }
  while (end > i && is_space(s[end - 1])) --end;
  return end;
}

void mask_entries(Scan& scan, std::string_view s, std::string& out, int depth);
void mask_bracketed(Scan& scan, std::string_view value, std::string& out, int depth);

// The items of an array body: inline tables and arrays inside are masked by key.
void mask_array(Scan& scan, std::string_view s, std::string& out, int depth) {
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (is_space(c) || c == ',' || is_line_break(c)) {
      out += c;
      ++i;
    } else if (c == '#') {
      const std::size_t stop = line_end(s, i);
      out.append(s.substr(i, stop - i));
      i = stop;
    } else {
      std::size_t stop = value_end(scan, s, i);
      if (stop == kNone) stop = line_end(s, i);
      if (stop <= i) stop = i + 1;
      const std::string_view item = s.substr(i, stop - i);
      if ((c == '{' || c == '[') && depth < kMaxDepth) {
        mask_bracketed(scan, item, out, depth + 1);
      } else {
        out.append(item);
      }
      i = stop;
    }
  }
}

// `value` is an array or inline table, closed or cut at the end of its first line.
void mask_bracketed(Scan& scan, std::string_view value, std::string& out, int depth) {
  const bool is_table = value.front() == '{';
  const bool closed = value.size() > 1 && value.back() == (is_table ? '}' : ']');
  const std::string_view body = value.substr(1, closed ? value.size() - 2 : value.size() - 1);
  out += value.front();
  if (is_table) {
    mask_entries(scan, body, out, depth);
  } else {
    mask_array(scan, body, out, depth);
  }
  if (closed) out += value.back();
}

// Reads a possibly dotted key at s[i] (`a.b`, `a."b"`, `"a" . 'b'`). Returns
// the index after it (and any spaces) and sets `last` to the content of its
// last part, or returns kNone when there is no key and sets `reached` to
// where the reading stopped: no key starts inside a run of dotted parts, so a
// caller resumes there, and a long run that ends in an empty part (`a.a.a.`) is
// read once, not once for each of its characters.
std::size_t read_key(std::string_view s, std::size_t i, std::string_view& last, std::size_t& reached) noexcept {
  std::size_t j = i;
  while (true) {
    while (j < s.size() && is_space(s[j])) ++j;
    std::size_t part_end = j;
    if (j < s.size() && (s[j] == '"' || s[j] == '\'')) {
      part_end = quoted_end(s, j);
      const bool closed = part_end - j >= 2 && s[part_end - 1] == s[j];
      last = s.substr(j + 1, part_end - j - (closed ? 2 : 1));
    } else {
      while (part_end < s.size() && is_bare_key_char(s[part_end])) ++part_end;
      if (part_end == j) {
        reached = j;
        return kNone;
      }
      last = s.substr(j, part_end - j);
    }
    std::size_t after = part_end;
    while (after < s.size() && is_space(s[after])) ++after;
    if (after < s.size() && s[after] == '.') {
      j = after + 1;
      continue;
    }
    return after;
  }
}

// Parses `key = value` at s[i]. On success appends it, masked, to `out` and
// returns the index after the value; otherwise returns kNone, appends nothing
// and sets `skip` to where the text that is no assignment ends (past i).
// `top`: a line of the file, not the body of an inline table.
std::size_t mask_assignment(Scan& scan, std::string_view s, std::size_t i, std::string& out, int depth, bool top,
                            std::size_t& skip) {
  std::string_view last;
  std::size_t reached = i;
  const std::size_t key_end = read_key(s, i, last, reached);
  skip = i + 1;
  if (key_end == kNone) {
    skip = reached > i ? reached : i + 1;
    return kNone;
  }
  if (key_end >= s.size() || s[key_end] != '=') {
    skip = key_end > i ? key_end : i + 1;
    return kNone;
  }
  std::size_t eq = key_end + 1;
  while (eq < s.size() && is_space(s[eq])) ++eq;
  const bool secret =
      is_secret_key(last_part(last)) || is_hook_secret(scan, s.substr(i, key_end - i), last_part(last));
  if (secret && top && (eq >= s.size() || is_line_break(s[eq]))) {
    // `password =` with the value on a later line: that line is masked whole.
    std::size_t next = eq;
    while (next < s.size() && (is_line_break(s[next]) || is_space(s[next]))) ++next;
    if (next >= s.size()) return kNone;
    std::size_t text = next;  // start of that line, keeping the blank lines before it
    while (text > eq && !is_line_break(s[text - 1])) --text;
    out.append(s.substr(i, text - i));
    out += kMask;
    return line_end(s, next);
  }
  out.append(s.substr(i, eq - i));
  const char c = eq < s.size() ? s[eq] : '\0';
  const bool bare_secret = secret && top && c != '"' && c != '\'' && c != '{' && c != '[';
  std::size_t end = bare_secret ? bare_secret_end(s, eq) : value_end(scan, s, eq);
  if (end == kNone) end = line_end(s, eq);  // not closed: the value is its first line
  const std::string_view value = s.substr(eq, end - eq);
  if (secret) {
    out += '"';
    out += kMask;
    out += '"';
  } else if (!value.empty() && (value.front() == '{' || value.front() == '[') && depth < kMaxDepth) {
    mask_bracketed(scan, value, out, depth + 1);
  } else {
    out.append(value);
  }
  return end;
}

// The body of an inline table: `key = value` pairs and their separators.
void mask_entries(Scan& scan, std::string_view s, std::string& out, int depth) {
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (is_space(c) || c == ',' || is_line_break(c)) {
      out += c;
      ++i;
    } else if (c == '#') {
      const std::size_t stop = line_end(s, i);
      out.append(s.substr(i, stop - i));
      i = stop;
    } else {
      std::size_t skip = i + 1;
      const std::size_t next = mask_assignment(scan, s, i, out, depth, false, skip);
      if (next == kNone) {
        out.append(s.substr(i, skip - i));  // not an assignment: copied as it is
        i = skip;
      } else {
        i = next > i ? next : i + 1;
      }
    }
  }
}

}  // namespace

bool is_secret_key(std::string_view key) noexcept {
  static constexpr std::array<std::string_view, 6> names{"password", "passwd", "token", "secret", "key", "credential"};
  if (!key.empty() && (key.back() == 's' || key.back() == 'S')) {
    const std::string_view singular = key.substr(0, key.size() - 1);
    for (const std::string_view name : names) {
      if (ends_with_word(singular, name)) return true;
    }
  }
  for (const std::string_view name : names) {
    if (ends_with_word(key, name)) return true;
  }
  return false;
}

namespace {

bool is_param_name_char(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

bool ends_param_value(char c) noexcept {
  return c == '&' || c == '#' || c == '"' || c == '\'' || std::isspace(static_cast<unsigned char>(c)) != 0;
}

// The password of every `scheme://user:password@host`.
std::string url_passwords(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t copied = 0;  // text before this is already in `out`
  std::size_t scheme = text.find("://");
  std::size_t pos = 0;
  while (pos < text.size() && scheme != kNone) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) ++pos;
    std::size_t end = pos;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0) ++end;
    // The whitespace-free token [pos, end): its userinfo runs from after the
    // first "://" in it to its last '@'. A quote does not end it (a password
    // may hold one). Only the first "://" can matter: a later one has fewer
    // characters before the same '@'.
    if (scheme < pos) scheme = text.find("://", pos);
    if (scheme != kNone && scheme + 3 <= end) {
      const std::size_t start = scheme + 3;
      const std::size_t at = text.substr(start, end - start).rfind('@');
      const std::size_t colon = at == kNone ? kNone : text.substr(start, at).find(':');
      if (colon != kNone) {
        out.append(text.substr(copied, start + colon + 1 - copied));
        out += kMask;
        copied = start + at;
      }
    }
    pos = end;
  }
  out.append(text.substr(copied));
  return out;
}

// The value of every `?name=value` and `&name=value` whose name is a secret
// key, up to the next '&', '#', quote or space. Each character is looked at
// once.
std::string query_secrets(std::string text) {
  std::size_t k = text.find_first_of("?&");
  std::string out;  // reserved at the first mask: most text has none
  std::size_t copied = 0;
  while (k != kNone) {
    std::size_t n = k + 1;
    while (n < text.size() && is_param_name_char(text[n])) ++n;
    std::size_t resume = n;
    if (n < text.size() && text[n] == '=' && n > k + 1 &&
        is_secret_key(std::string_view(text).substr(k + 1, n - k - 1))) {
      std::size_t v = n + 1;
      while (v < text.size() && !ends_param_value(text[v])) ++v;
      if (v > n + 1) {
        if (copied == 0) out.reserve(text.size());
        out.append(text, copied, n + 1 - copied);
        out += kMask;
        copied = v;
      }
      resume = v;
    }
    k = text.find_first_of("?&", resume);
  }
  if (copied == 0) return text;
  out.append(text, copied, kNone);
  return out;
}

}  // namespace

std::string in_text(std::string_view text) { return query_secrets(url_passwords(text)); }

std::string in_toml(std::string_view toml) {
  std::string masked;
  masked.reserve(toml.size());
  Scan scan;
  scan.limit = 4 * toml.size() + 65536;
  std::size_t pos = 0;
  if (toml.starts_with("\xEF\xBB\xBF")) {  // a byte order mark stays; the first key is read after it
    masked.append(toml.substr(0, 3));
    pos = 3;
  }
  while (pos < toml.size()) {
    std::size_t first = pos;
    while (first < toml.size() && is_space(toml[first])) ++first;
    std::size_t next = kNone;
    if (first < toml.size() && toml[first] == '[') {
      const std::size_t eol_header = line_end(toml, first);
      const std::string_view line = toml.substr(first, eol_header - first);
      scan.hook_table = contains_nocase(line.substr(0, line.find(']')), "hook");
    }
    if (first < toml.size() && toml[first] != '#' && toml[first] != '[' && !is_line_break(toml[first])) {
      const std::size_t mark = masked.size();
      masked.append(toml.substr(pos, first - pos));
      std::size_t skip = 0;
      next = mask_assignment(scan, toml, first, masked, 0, true, skip);
      if (next == kNone) masked.resize(mark);
    }
    // The rest of the line (spaces, a comment, a '\r') and its newline are kept.
    const std::size_t from = next == kNone ? pos : next;
    const std::size_t eol = toml.find('\n', from);
    const std::size_t stop = eol == kNone ? toml.size() : eol + 1;
    masked.append(toml.substr(from, stop - from));
    pos = stop;
  }
  return in_text(masked);
}

}  // namespace nobion::redact
