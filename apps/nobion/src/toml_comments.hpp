#pragma once

// Whether a TOML text has comments: the editors that write a canonical file
// (conditionals, step heat templates) ask before a save drops them.

#include <cstddef>
#include <string>

namespace nobion::ui {

// A '#' outside a string: a comment the canonical file would not have.
[[nodiscard]] inline bool has_toml_comment(const std::string& text) {
  char quote = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char ch = text[i];
    if (quote != 0) {
      if (ch == '\\' && quote == '"') ++i;
      else if (ch == quote || ch == '\n') quote = 0;
    } else if (ch == '"' || ch == '\'') {
      quote = ch;
    } else if (ch == '#') {
      return true;
    }
  }
  return false;
}

}  // namespace nobion::ui
