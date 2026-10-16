#pragma once

#include <string>
#include <string_view>

// Canonical TOML scalar text shared by the library's writers (queue files,
// step heat templates). Private to libs/experiment.
namespace pychron::experiment::detail {

// A basic (double-quoted) TOML string, control characters escaped.
[[nodiscard]] std::string toml_string(std::string_view s);

// The shortest text that parses back to the same double, always a TOML float
// ("30.0", not "30"), exponent form only for very large or small magnitudes.
[[nodiscard]] std::string toml_float(double v);

}  // namespace pychron::experiment::detail
