// Compiled into a separate static library that nothing references by symbol.
// Its REGISTER_DRIVER must still run: see nobion_link_drivers().

#include "nobion/devices/capabilities.hpp"
#include "nobion/devices/driver_registry.hpp"

namespace {

class ArchivedDriver : public nobion::Device, public nobion::IScannable {
 public:
  explicit ArchivedDriver(std::string name) : Device(std::move(name)) {}

  static nobion::DriverSchema schema() { return {"", "linker fixture", {}}; }
  static nobion::Result<std::unique_ptr<ArchivedDriver>> create(const nobion::DriverArgs& args) {
    return std::make_unique<ArchivedDriver>(args.name);
  }

  nobion::Result<nobion::Sample> sample() override { return nobion::Sample{name(), {}, 0.0}; }
};

}  // namespace

REGISTER_DRIVER("test_archived_driver", ArchivedDriver);
