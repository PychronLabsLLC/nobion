// The in-memory fake passes every extraction conformance suite, so the suites
// themselves are known to be satisfiable.

#include "extraction/conformance.hpp"
#include "extraction/fake.hpp"

namespace {

using nobion::extraction::Capability;
using nobion::extraction::CapabilitySet;
using nobion::extraction::testing::FakeExtractionDevice;

struct FakeHarness {
  FakeExtractionDevice fake{"fake-laser",
                            CapabilitySet{Capability::Laser, Capability::Stage,
                                          Capability::Pattern, Capability::Furnace}};
  nobion::extraction::IExtractionDevice& device() { return fake; }
};

struct BareHarness {
  FakeExtractionDevice fake{"bare"};
  nobion::extraction::IExtractionDevice& device() { return fake; }
};

using Harnesses = ::testing::Types<FakeHarness, BareHarness>;
INSTANTIATE_TYPED_TEST_SUITE_P(Fake, ExtractionDeviceConformance, Harnesses);
INSTANTIATE_TYPED_TEST_SUITE_P(Fake, LaserConformance, ::testing::Types<FakeHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Fake, StageConformance, ::testing::Types<FakeHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Fake, PatternConformance, ::testing::Types<FakeHarness>);
INSTANTIATE_TYPED_TEST_SUITE_P(Fake, FurnaceConformance, ::testing::Types<FakeHarness>);

}  // namespace
