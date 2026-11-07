#include "domain.hpp"

#include <algorithm>
#include <ostream>
#include <string>
#include <vector>

#include "nobion/arar/domain.hpp"
#include "nobion/setup/site.hpp"

namespace elctl {

namespace {

std::string join(const std::vector<std::string>& ids) {
  std::string text;
  for (const auto& id : ids) text += (text.empty() ? "" : ", ") + id;
  return text;
}

}  // namespace

const nobion::processing::Domain* choose_domain(nobion::processing::DomainRegistry& registry, std::string_view id,
                                                 std::ostream& err) {
  registry.add(nobion::processing::make_noble_gas_domain());
  registry.add(nobion::arar::make_domain());
  auto known = nobion::setup::known_domains();
  const auto registered = registry.ids();  // sorted
  std::sort(known.begin(), known.end());
  if (known != registered) {
    err << "error: setup::known_domains() lists " << join(known) << " but the registry holds " << join(registered)
        << '\n';
    return nullptr;
  }
  if (const auto* domain = registry.find(id)) return domain;
  err << "error: unknown domain '" << id << "'; known: " << join(registered) << '\n';
  return nullptr;
}

}  // namespace elctl
