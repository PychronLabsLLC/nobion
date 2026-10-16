#include "toml_text.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pychron::experiment::detail {

// Canonical scalar text: basic (double-quoted) strings and the shortest float
// that parses back to the same double, so dumps are stable and diff cleanly.
std::string toml_string(std::string_view s) {
  std::string out = "\"";
  for (char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\t': out += "\\t"; break;
      case '\r': out += "\\r"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20 || c == 0x7f) {
          std::array<char, 8> buf{};
          std::snprintf(buf.data(), buf.size(), "\\u%04X", c);
          out += buf.data();
        } else {
          out += ch;
        }
    }
  }
  return out + "\"";
}

std::string toml_float(double v) {
  if (std::isnan(v)) return "nan";
  if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
  // Exponent form only for very large or small magnitudes ("30.0", not "3e+01").
  const double mag = std::fabs(v);
  const bool plain = mag == 0 || (mag >= 1e-5 && mag < 1e16);
  std::array<char, 32> buf{};
  for (int precision = 1; precision <= 17; ++precision) {
    std::snprintf(buf.data(), buf.size(), "%.*g", precision, v);
    if (std::strtod(buf.data(), nullptr) == v && !(plain && std::strchr(buf.data(), 'e'))) break;
  }
  std::string s = buf.data();
  std::replace(s.begin(), s.end(), ',', '.');  // decimal-comma locales
  if (s.find_first_of(".e") == std::string::npos) s += ".0";  // keep it a TOML float
  return s;
}

}  // namespace pychron::experiment::detail
