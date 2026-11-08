#include "support_bundle_cmd.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "duration.hpp"
#include "nobion/core/process_log.hpp"
#include "nobion/setup/profile.hpp"
#include "nobion/setup/installer.hpp"
#include "nobion/setup/site.hpp"
#include "nobion/setup/support_bundle.hpp"
#include "setup.hpp"

namespace elctl {

namespace {

namespace fs = std::filesystem;

constexpr const char* kSupportBundleUsage =
    "usage: elctl support-bundle [--install NAME] [--out FILE] [--since 72h] [--no-traces]\n";

}  // namespace

int support_bundle_command(const std::vector<std::string>& args, std::optional<std::string> install, Io io) {
  nobion::setup::BundleOptions options;
  auto usage = [&](const std::string& message) {
    io.err << "elctl support-bundle: " << message << "\n" << kSupportBundleUsage;
    return static_cast<int>(kUsage);
  };
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& a = args[i];
    if (a == "--no-traces") {
      options.include_traces = false;
    } else if (a == "--install" || a == "--out" || a == "--since") {
      if (i + 1 >= args.size()) return usage(a + " needs a value");
      const std::string& value = args[++i];
      if (a == "--install") {
        install = value;
      } else if (a == "--out") {
        options.out = fs::path(value);
      } else {
        const auto since = parse_duration(value);
        if (!since) return usage("--since: " + since.error().what);
        // Whole hours, rounded up: --since 90m keeps two hours rather than one.
        options.logs_since = std::chrono::ceil<std::chrono::hours>(*since);
      }
    } else {
      return usage("unexpected '" + a + "'");
    }
  }

  // The install named, else the default one. With nothing installed (or a
  // site config that does not load: just when a bundle is wanted) it is
  // written without one; a name that is not there, or several installs and
  // no default, is a mistake to fix first.
  std::optional<nobion::setup::SiteInstall> chosen;
  const fs::path site_path = nobion::setup::default_site_path();
  if (auto site = nobion::setup::load_site(site_path); !site) {
    if (install) {
      io.err << "error: " << site.error().what << "\n";
      return kFailed;
    }
    io.err << "note: " << site.error().what << "; the bundle holds no install\n";
  } else if (const auto* picked = site->pick(install)) {
    chosen = *picked;
  } else if (install || !site->installs.empty()) {
    // resolve_install says which: no such name, or several and no default.
    const auto why = resolve_install(install);
    io.err << "error: " << (why ? "no install chosen" : why.error().what) << "\n";
    return kFailed;
  } else {
    io.err << "note: nothing is installed; the bundle holds no install\n";
  }

  // Doctor as `elctl doctor` runs it, never with --probe.
  const auto resources = nobion::setup::find_resources();
  const auto library = nobion::setup::ProfileLibrary::load(resources.profiles, resources.examples);
  if (library) options.doctor.library = &*library;
#ifdef NOBION_ELCTL_HAS_STORE
  options.doctor.open_database = [](const std::string& url) { return open_store(url, false); };
#endif
  // The process's hub: its log folder, and a fresh flight recorder dump.
  options.hub = nobion::process_log_hub();
  options.site_path = site_path;

  const auto written = nobion::setup::write_support_bundle(chosen ? &*chosen : nullptr, options);
  if (!written) {
    io.err << "error: " << written.error().what << "\n";
    return kFailed;
  }
  io.out << written->string() << "\n";
  return kOk;
}

}  // namespace elctl
