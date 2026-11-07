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

bool is_space(char c) noexcept { return c == ' ' || c == '\t'; }
bool is_line_break(char c) noexcept { return c == '\n' || c == '\r'; }
bool is_upper(char c) noexcept { return std::isupper(static_cast<unsigned char>(c)) != 0; }
bool is_lower(char c) noexcept { return std::islower(static_cast<unsigned char>(c)) != 0; }

bool is_bare_key_char(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
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

// One past the end of the quoted string starting at s[i].
std::size_t quoted_end(std::string_view s, std::size_t i) noexcept {
  const char q = s[i];
  for (std::size_t j = i + 1; j < s.size(); ++j) {
    if (q == '"' && s[j] == '\\') {
      ++j;
    } else if (s[j] == q || s[j] == '\n') {
      return s[j] == q ? j + 1 : j;
    }
  }
  return s.size();
}

bool is_triple(std::string_view s, std::size_t i) noexcept {
  return i + 2 < s.size() && s[i + 1] == s[i] && s[i + 2] == s[i];
}

// One past the closing triple quote of the string starting at s[i] (`"""` or `'''`).
std::size_t triple_end(std::string_view s, std::size_t i) noexcept {
  const char q = s[i];
  for (std::size_t j = i + 3; j < s.size(); ++j) {
    if (q == '"' && s[j] == '\\') {
      ++j;
    } else if (s[j] == q && s.substr(j, 3) == std::string_view{s.data() + i, 3}) {
      j += 3;
      while (j < s.size() && s[j] == q) ++j;  // up to two quotes may belong to the content
      return j;
    }
  }
  return s.size();
}

// One past the end of the TOML value starting at s[i]: a string (multi-line
// too), an array or inline table (across lines), or a bare value.
std::size_t value_end(std::string_view s, std::size_t i) noexcept {
  if (i >= s.size()) return s.size();
  const char c = s[i];
  if (c == '"' || c == '\'') {
    return is_triple(s, i) ? triple_end(s, i) : quoted_end(s, i);
  }
  if (c == '{' || c == '[') {
    int depth = 0;
    for (std::size_t j = i; j < s.size(); ++j) {
      const char d = s[j];
      if (d == '"' || d == '\'') {
        j = (is_triple(s, j) ? triple_end(s, j) : quoted_end(s, j)) - 1;
      } else if (d == '#') {
        while (j + 1 < s.size() && s[j + 1] != '\n') ++j;
      } else if (d == '{' || d == '[') {
        ++depth;
      } else if (d == '}' || d == ']') {
        if (--depth == 0) return j + 1;
      }
    }
    return s.size();
  }
  std::size_t j = i;
  while (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' && s[j] != '#' && !is_line_break(s[j])) ++j;
  while (j > i && is_space(s[j - 1])) --j;
  return j;
}

void mask_entries(std::string_view s, std::string& out);

// The items of an array body: inline tables and arrays inside are masked by key.
void mask_array(std::string_view s, std::string& out) {
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (is_space(c) || c == ',' || is_line_break(c)) {
      out += c;
      ++i;
    } else if (c == '#') {
      const std::size_t eol = s.find('\n', i);
      const std::size_t stop = eol == kNone ? s.size() : eol;
      out.append(s.substr(i, stop - i));
      i = stop;
    } else {
      const std::size_t end = value_end(s, i);
      const std::size_t stop = end > i ? end : i + 1;
      const std::string_view item = s.substr(i, stop - i);
      const bool closed = item.size() > 1 && (c == '{' ? item.back() == '}' : item.back() == ']');
      if (c == '{') {
        out += '{';
        mask_entries(item.substr(1, closed ? item.size() - 2 : item.size() - 1), out);
        if (closed) out += '}';
      } else if (c == '[') {
        out += '[';
        mask_array(item.substr(1, closed ? item.size() - 2 : item.size() - 1), out);
        if (closed) out += ']';
      } else {
        out.append(item);
      }
      i = stop;
    }
  }
}

// Parses `key = value` at s[i]. On success appends it, masked, to `out` and
// returns the index after the value; otherwise returns kNone and appends nothing.
std::size_t mask_assignment(std::string_view s, std::size_t i, std::string& out) {
  std::size_t key_end = i;
  std::string_view key;
  if (s[i] == '"' || s[i] == '\'') {
    key_end = quoted_end(s, i);
    const bool closed = key_end - i >= 2 && s[key_end - 1] == s[i];
    key = s.substr(i + 1, key_end - i - (closed ? 2 : 1));
  } else {
    while (key_end < s.size() && is_bare_key_char(s[key_end])) ++key_end;
    key = s.substr(i, key_end - i);
  }
  std::size_t eq = key_end;
  while (eq < s.size() && is_space(s[eq])) ++eq;
  if (key_end == i || eq >= s.size() || s[eq] != '=') return kNone;
  ++eq;
  while (eq < s.size() && is_space(s[eq])) ++eq;
  out.append(s.substr(i, eq - i));
  const std::size_t end = value_end(s, eq);
  const std::string_view value = s.substr(eq, end - eq);
  if (is_secret_key(last_part(key))) {
    out += '"';
    out += kMask;
    out += '"';
  } else if (!value.empty() && (value.front() == '{' || value.front() == '[')) {
    const bool is_table = value.front() == '{';
    const bool closed = value.size() > 1 && value.back() == (is_table ? '}' : ']');
    const std::string_view body = value.substr(1, closed ? value.size() - 2 : value.size() - 1);
    out += value.front();
    if (is_table) {
      mask_entries(body, out);
    } else {
      mask_array(body, out);
    }
    if (closed) out += value.back();
  } else {
    out.append(value);
  }
  return end;
}

// The body of an inline table: `key = value` pairs and their separators.
void mask_entries(std::string_view s, std::string& out) {
  std::size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (is_space(c) || c == ',' || is_line_break(c)) {
      out += c;
      ++i;
    } else if (c == '#') {
      const std::size_t eol = s.find('\n', i);
      const std::size_t stop = eol == kNone ? s.size() : eol;
      out.append(s.substr(i, stop - i));
      i = stop;
    } else {
      const std::size_t next = mask_assignment(s, i, out);
      if (next == kNone) {
        out += c;  // not an assignment: copy a character and look again
        ++i;
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

std::string in_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t copied = 0;  // text before this is already in `out`
  std::size_t search = 0;
  while (true) {
    const std::size_t scheme = text.find("://", search);
    if (scheme == kNone) break;
    const std::size_t start = scheme + 3;
    // The token runs to whitespace or a quote; its userinfo to the last '@'.
    std::size_t end = start;
    while (end < text.size() && std::isspace(static_cast<unsigned char>(text[end])) == 0 && text[end] != '"' &&
           text[end] != '\'') {
      ++end;
    }
    const std::string_view token = text.substr(start, end - start);
    const std::size_t at = token.rfind('@');
    const std::size_t colon = at == kNone ? kNone : token.find(':');
    if (colon != kNone && colon < at) {
      out.append(text.substr(copied, start + colon + 1 - copied));
      out += kMask;
      copied = start + at;
      search = copied;
    } else {
      search = start;
    }
  }
  out.append(text.substr(copied));
  return out;
}

std::string in_toml(std::string_view toml) {
  std::string masked;
  masked.reserve(toml.size());
  std::size_t pos = 0;
  while (pos < toml.size()) {
    std::size_t first = pos;
    while (first < toml.size() && is_space(toml[first])) ++first;
    std::size_t next = kNone;
    if (first < toml.size() && toml[first] != '#' && toml[first] != '[' && !is_line_break(toml[first])) {
      masked.append(toml.substr(pos, first - pos));
      next = mask_assignment(toml, first, masked);
      if (next == kNone) masked.erase(masked.size() - (first - pos));
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
