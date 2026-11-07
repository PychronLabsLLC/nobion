#include "nobion/core/log_summary.hpp"

#include "nobion/core/redact.hpp"

namespace nobion {

std::string truncate_for_log(std::string_view text, std::size_t max) {
  if (text.size() <= max) return std::string{text};
  std::size_t cut = max;
  // Back up off a continuation byte (10xxxxxx) so the next byte starts a character.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) --cut;
  std::string out{text.substr(0, cut)};
  out += "\xE2\x80\xA6";
  return out;
}

std::string summarize(std::span<const std::pair<std::string, std::string>> fields) {
  std::string out;
  for (const auto& [key, value] : fields) {
    if (!out.empty()) out += ", ";
    out += key;
    out += '=';
    out += redact::is_secret_key(key) ? redact::kMask : std::string_view{value};
  }
  return truncate_for_log(out);
}

}  // namespace nobion
