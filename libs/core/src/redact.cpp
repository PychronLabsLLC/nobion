#include "nobion/core/redact.hpp"

#include <array>
#include <cctype>
#include <cstddef>

namespace nobion::redact {
namespace {

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

bool is_bare_key_char(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

// End (one past) of the quoted string starting at s[i].
std::size_t quoted_end(std::string_view s, std::size_t i) noexcept {
  const char q = s[i];
  for (std::size_t j = i + 1; j < s.size(); ++j) {
    if (q == '"' && s[j] == '\\') {
      ++j;
    } else if (s[j] == q) {
      return j + 1;
    }
  }
  return s.size();
}

// End (one past) of the TOML value starting at s[i], not past a trailing comment.
std::size_t value_end(std::string_view s, std::size_t i) noexcept {
  if (i >= s.size()) return s.size();
  if (s[i] == '"' || s[i] == '\'') return quoted_end(s, i);
  if (s[i] == '{' || s[i] == '[') {
    int depth = 0;
    for (std::size_t j = i; j < s.size(); ++j) {
      const char c = s[j];
      if (c == '"' || c == '\'') {
        j = quoted_end(s, j) - 1;
      } else if (c == '{' || c == '[') {
        ++depth;
      } else if (c == '}' || c == ']') {
        if (--depth == 0) return j + 1;
      }
    }
    return s.size();
  }
  std::size_t j = i;
  while (j < s.size() && s[j] != ',' && s[j] != '}' && s[j] != ']' && s[j] != '#') ++j;
  while (j > i && is_space(s[j - 1])) --j;
  return j;
}

// `key = value` pairs of a line or of an inline table's body.
void mask_entries(std::string_view s, std::string& out) {
  std::size_t i = 0;
  while (i < s.size()) {
    // Separators, spaces and a comment.
    while (i < s.size() && (is_space(s[i]) || s[i] == ',')) out += s[i++];
    if (i >= s.size()) break;
    if (s[i] == '#') {
      out.append(s.substr(i));
      return;
    }
    std::size_t key_end = i;
    std::string_view key;
    if (s[i] == '"' || s[i] == '\'') {
      key_end = quoted_end(s, i);
      key = s.substr(i + 1, key_end - i - (key_end - i >= 2 ? 2 : 1));
    } else {
      while (key_end < s.size() && is_bare_key_char(s[key_end])) ++key_end;
      key = s.substr(i, key_end - i);
      // A dotted key is secret by its last part.
      if (const std::size_t dot = key.rfind('.'); dot != std::string_view::npos) key = key.substr(dot + 1);
    }
    std::size_t eq = key_end;
    while (eq < s.size() && is_space(s[eq])) ++eq;
    if (key_end == i || eq >= s.size() || s[eq] != '=') {
      // Not an assignment (array item, header, stray text): copy to the next separator.
      const std::size_t stop = key_end == i ? i + 1 : key_end;
      out.append(s.substr(i, stop - i));
      i = stop;
      continue;
    }
    ++eq;
    while (eq < s.size() && is_space(s[eq])) ++eq;
    out.append(s.substr(i, eq - i));
    const std::size_t end = value_end(s, eq);
    const std::string_view value = s.substr(eq, end - eq);
    if (is_secret_key(key)) {
      out += '"';
      out += kMask;
      out += '"';
    } else if (!value.empty() && value.front() == '{') {
      out += '{';
      mask_entries(value.substr(1, value.back() == '}' ? value.size() - 2 : value.size() - 1), out);
      if (value.back() == '}') out += '}';
    } else {
      out.append(value);
    }
    i = end > eq ? end : eq + 1;
    if (end == eq && eq - 1 < s.size()) out.append(s.substr(eq, 1));
  }
}

}  // namespace

bool is_secret_key(std::string_view key) noexcept {
  static constexpr std::array<std::string_view, 6> names{"password", "passwd", "token", "secret", "key", "credential"};
  for (const std::string_view name : names) {
    if (equals_nocase(key, name)) return true;
    if (key.size() > name.size()) {
      const std::string_view tail = key.substr(key.size() - name.size());
      const char sep = key[key.size() - name.size() - 1];
      if ((sep == '_' || sep == '-') && equals_nocase(tail, name)) return true;
    }
  }
  return false;
}

std::string in_text(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t scheme = text.find("://", pos);
    if (scheme == std::string_view::npos) break;
    const std::size_t start = scheme + 3;
    std::size_t end = start;
    while (end < text.size() && text[end] != '/' && text[end] != '?' && text[end] != '#' &&
           std::isspace(static_cast<unsigned char>(text[end])) == 0 && text[end] != '"' && text[end] != '\'') {
      ++end;
    }
    const std::string_view authority = text.substr(start, end - start);
    const std::size_t at = authority.rfind('@');
    const std::size_t colon = at == std::string_view::npos ? std::string_view::npos : authority.find(':');
    if (colon != std::string_view::npos && colon < at) {
      out.append(text.substr(pos, start + colon + 1 - pos));
      out += kMask;
      out.append(text.substr(start + at, end - start - at));
    } else {
      out.append(text.substr(pos, end - pos));
    }
    pos = end;
  }
  out.append(text.substr(pos));
  return out;
}

std::string in_toml(std::string_view toml) {
  std::string masked;
  masked.reserve(toml.size());
  std::size_t pos = 0;
  while (pos <= toml.size()) {
    std::size_t nl = toml.find('\n', pos);
    if (nl == std::string_view::npos) nl = toml.size();
    const std::string_view line = toml.substr(pos, nl - pos);
    std::size_t first = 0;
    while (first < line.size() && is_space(line[first])) ++first;
    if (first < line.size() && (line[first] == '#' || line[first] == '[')) {
      masked.append(line);
    } else {
      mask_entries(line, masked);
    }
    if (nl < toml.size()) masked += '\n';
    pos = nl + 1;
  }
  return in_text(masked);
}

}  // namespace nobion::redact
