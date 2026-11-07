// toml++ is compiled here once (TOML_HEADER_ONLY=0, set publicly on nobion_core)
// instead of in each of the hundreds of objects its header reaches through
// system_config.hpp.
#define TOML_IMPLEMENTATION
#include <toml++/toml.hpp>
