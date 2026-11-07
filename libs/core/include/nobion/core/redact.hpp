#pragma once

#include <string>
#include <string_view>

// One rule for what never leaves the machine in a log summary or a support
// bundle: values of secret-looking keys, and the password of a URL.
namespace nobion::redact {

inline constexpr std::string_view kMask = "***";

// True when `key` is a secret word (password, passwd, token, secret, key,
// credential) or ends in one after '_' or '-' (`DB_Password`, `api_key`).
// Case-insensitive. A word in the middle or glued on (`keyboard`, `monkey`,
// `tokenizer`) does not count.
[[nodiscard]] bool is_secret_key(std::string_view key) noexcept;

// Masks the password of every `scheme://user:password@host` in `text`.
[[nodiscard]] std::string in_text(std::string_view text);

// Line-based TOML: the value of every `key = value` whose key is secret
// becomes `"***"` (inside an inline table too); comments and section headers
// stay; then `in_text` over the result.
[[nodiscard]] std::string in_toml(std::string_view toml);

}  // namespace nobion::redact
