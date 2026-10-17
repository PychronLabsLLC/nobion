#include "pychron/entry/names.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <regex>

namespace pychron::entry {
namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }

// "<digits>" + 1 in decimal, kept at least as wide as the input.
std::string increment_digits(const std::string& digits) {
  std::string out = digits;
  int i = static_cast<int>(out.size()) - 1;
  for (; i >= 0; --i) {
    if (out[static_cast<std::size_t>(i)] == '9') {
      out[static_cast<std::size_t>(i)] = '0';
    } else {
      ++out[static_cast<std::size_t>(i)];
      break;
    }
  }
  if (i < 0) out.insert(out.begin(), '1');
  return out;
}

// `text` without a leading "https://<host>/" or "http://<host>/".
std::string_view strip_url(std::string_view text, std::string_view host) {
  for (const std::string_view scheme : {"https://", "http://"}) {
    if (text.starts_with(scheme) && text.substr(scheme.size()).starts_with(host) &&
        text.substr(scheme.size() + host.size()).starts_with('/'))
      return text.substr(scheme.size() + host.size() + 1);
  }
  return text;
}

// ROR's alphabet: digits and lower-case letters, less i, l, o and u.
bool is_ror_char(char c) {
  if (is_digit(c)) return true;
  return c >= 'a' && c <= 'z' && c != 'i' && c != 'l' && c != 'o' && c != 'u';
}

bool is_ascii(char c) { return static_cast<unsigned char>(c) < 0x80; }
bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
bool is_ascii_letter(char c) { return is_upper(c) || (c >= 'a' && c <= 'z'); }
// Any byte of a letter outside ASCII counts as a letter: the text is UTF-8 and
// which of those characters are letters is not for this code to say.
bool is_letter(char c) { return is_ascii_letter(c) || !is_ascii(c); }

// A name as it may be written: words of letters, with ' - and . inside or
// after them, one space between words. Nullopt when it is empty, does not
// begin with a letter, or holds anything else (a digit, a second comma).
std::optional<std::string> name_words(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (is_space(c)) {
      if (c != ' ') return std::nullopt;
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else if (is_letter(c) || c == '\'' || c == '-' || c == '.') {
      out += c;
    } else {
      return std::nullopt;
    }
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty() || !is_letter(out.front())) return std::nullopt;
  return out;
}

}  // namespace

std::string trim(std::string_view text) {
  std::size_t b = 0, e = text.size();
  while (b < e && is_space(text[b])) ++b;
  while (e > b && is_space(text[e - 1])) --e;
  return std::string(text.substr(b, e - b));
}

Result<PiName> parse_pi(std::string_view text, const std::vector<std::string>& allowed) {
  const std::string t = trim(text);
  if (std::find(allowed.begin(), allowed.end(), t) != allowed.end()) return PiName{t, ""};
  const auto refuse = [&t] {
    return fail(ErrorKind::Config, "principal investigator '" + t + R"(': write "Family" or "Family, Given")");
  };
  const auto comma = t.find(',');
  const auto family = name_words(std::string_view(t).substr(0, comma));
  if (!family || family->size() < 2) return refuse();
  // A name all in small ASCII letters is a slip of the hand ("ross"), not a particle ("van der Pluijm").
  if (std::none_of(family->begin(), family->end(), [](char c) { return is_upper(c) || !is_ascii(c); }))
    return refuse();
  if (comma == std::string::npos) return PiName{*family, ""};
  auto given = name_words(std::string_view(t).substr(comma + 1));
  if (!given) return refuse();
  // A lone initial with its full stop ("J.") is the initial ("J").
  if (given->size() == 2 && is_ascii_letter(given->front()) && given->back() == '.') given->pop_back();
  return PiName{*family, *given};
}

Result<PiName> parse_organisation(std::string_view text) {
  std::string t = trim(text);
  if (t.empty()) return fail(ErrorKind::Config, "an organisation needs a name");
  return PiName{std::move(t), "", persistence::PiKind::Organisation};
}

Result<void> check_pi(const PiName& name, const std::optional<std::string>& orcid) {
  if (name.kind != persistence::PiKind::Organisation) return {};
  if (!name.given_name.empty())
    return fail(ErrorKind::Config, "organisation '" + name.family_name + "' has no given name");
  if (orcid) return fail(ErrorKind::Config, "organisation '" + name.family_name + "' has no ORCID; a person has one");
  return {};
}

std::string sheet_name(const PiName& name) {
  return name.given_name.empty() ? name.family_name : name.family_name + ", " + name.given_name;
}

std::string display_name(const PiName& name) {
  if (name.kind == persistence::PiKind::Organisation || name.given_name.empty()) return name.family_name;
  return name.given_name + " " + name.family_name;
}

std::string initial_of(std::string_view given) {
  if (given.empty()) return {};
  const char first = given.front();
  if (is_ascii(first)) return std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(first))));
  std::size_t n = 1;
  while (n < given.size() && (static_cast<unsigned char>(given[n]) & 0xC0U) == 0x80U) ++n;
  return std::string(given.substr(0, n));
}

Result<std::string> parse_orcid(std::string_view text) {
  const std::string t = trim(text);
  const std::string_view id = strip_url(t, "orcid.org");
  const auto refuse = [&] {
    return fail(ErrorKind::Config, "'" + t + "' is not an ORCID iD (0000-0002-1825-0097)");
  };
  if (id.size() != 19) return refuse();
  int total = 0;  // ISO 7064 mod 11-2 over the first 15 digits
  for (std::size_t i = 0; i < id.size(); ++i) {
    const char c = id[i];
    if (i % 5 == 4) {
      if (c != '-') return refuse();
    } else if (i == 18) {
      const int remainder = total % 11;
      const int check = (12 - remainder) % 11;
      if (c != (check == 10 ? 'X' : static_cast<char>('0' + check))) return refuse();
    } else {
      if (!is_digit(c)) return refuse();
      total = (total + (c - '0')) * 2;
    }
  }
  return std::string(id);
}

Result<std::string> parse_ror(std::string_view text) {
  const std::string t = trim(text);
  const std::string_view id = strip_url(t, "ror.org");
  const bool ok = id.size() == 9 && id[0] == '0' &&
                  std::all_of(id.begin() + 1, id.begin() + 7, is_ror_char) && is_digit(id[7]) && is_digit(id[8]);
  if (!ok) return fail(ErrorKind::Config, "'" + t + "' is not a ROR id (03yrm5c26)");
  return std::string(id);
}

bool valid_project_name(std::string_view name) {
  static const std::regex pattern(R"(^[A-Za-z][-A-Za-z0-9_]*$)");
  const std::string t(name);
  return std::regex_match(t, pattern);
}

bool valid_package_name(std::string_view name) {
  return !name.empty() && std::none_of(name.begin(), name.end(), is_space);
}

std::string next_package_name(const std::vector<std::string>& existing, std::string_view prefix) {
  std::string best;  // digits of the largest number, as written
  for (const auto& name : existing) {
    if (name.size() <= prefix.size() || !name.starts_with(prefix)) continue;
    const std::string digits = name.substr(prefix.size());
    if (!std::all_of(digits.begin(), digits.end(), is_digit)) continue;
    const auto value = [](const std::string& d) {
      const auto first = d.find_first_not_of('0');
      return first == std::string::npos ? std::string() : d.substr(first);
    };
    const std::string a = value(digits), b = value(best);
    if (best.empty() || a.size() > b.size() || (a.size() == b.size() && a > b) ||
        (a == b && digits.size() > best.size()))
      best = digits;
  }
  if (best.empty()) return std::string(prefix) + "001";
  std::string next = increment_digits(best);
  if (next.size() < 3) next.insert(0, 3 - next.size(), '0');
  return std::string(prefix) + next;
}

std::string next_letters(std::string_view letters) {
  if (letters.empty() || !std::all_of(letters.begin(), letters.end(), [](char c) { return c >= 'A' && c <= 'Z'; }))
    return "A";
  std::string out(letters);
  int i = static_cast<int>(out.size()) - 1;
  for (; i >= 0; --i) {
    if (out[static_cast<std::size_t>(i)] == 'Z') {
      out[static_cast<std::size_t>(i)] = 'A';
    } else {
      ++out[static_cast<std::size_t>(i)];
      break;
    }
  }
  if (i < 0) out.insert(out.begin(), 'A');
  return out;
}

std::string next_level_name(const std::vector<std::string>& existing) {
  std::string best;
  for (const auto& name : existing) {
    if (name.empty() || !std::all_of(name.begin(), name.end(), [](char c) { return c >= 'A' && c <= 'Z'; })) continue;
    if (best.empty() || name.size() > best.size() || (name.size() == best.size() && name > best)) best = name;
  }
  return best.empty() ? "A" : next_letters(best);
}

bool valid_packet(std::string_view packet) {
  std::size_t i = 0;
  while (i < packet.size() && std::isalpha(static_cast<unsigned char>(packet[i]))) ++i;
  if (i == packet.size()) return false;
  return std::all_of(packet.begin() + static_cast<std::ptrdiff_t>(i), packet.end(), is_digit);
}

std::optional<std::string> next_packet(std::string_view packet) {
  if (!valid_packet(packet)) return std::nullopt;
  std::size_t i = 0;
  while (std::isalpha(static_cast<unsigned char>(packet[i]))) ++i;
  return std::string(packet.substr(0, i)) + increment_digits(std::string(packet.substr(i)));
}

}  // namespace pychron::entry
