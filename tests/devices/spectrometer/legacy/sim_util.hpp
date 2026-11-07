#pragma once

// Shared helpers for legacy spectrometer driver tests.

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include <toml++/toml.hpp>

#include "nobion/transport/sim_transport.hpp"

namespace legacy_test {

using namespace std::chrono_literals;

inline nobion::TransportOptions bus_options(std::string name = "legacy_bus") {
  nobion::TransportOptions o;
  o.name = std::move(name);
  o.timeout = 50ms;
  return o;
}

inline std::unique_ptr<nobion::SimTransport> opened(std::unique_ptr<nobion::SimTransport> sim) {
  EXPECT_TRUE(sim->open());
  return sim;
}

inline std::unique_ptr<nobion::SimTransport> open_scripted(std::vector<nobion::SimStep> steps) {
  return opened(nobion::SimTransport::scripted(std::move(steps), bus_options()));
}

inline std::unique_ptr<nobion::SimTransport> open_hooked(nobion::SimTransport::Hook hook) {
  return opened(nobion::SimTransport::hooked(std::move(hook), bus_options()));
}

inline nobion::SimStep step(std::string_view tx, std::string_view rx) {
  return nobion::SimStep{nobion::to_bytes(tx), nobion::to_bytes(rx), {}};
}

inline nobion::SimStep write_only(std::string_view tx) { return nobion::SimStep{nobion::to_bytes(tx), {}, {}}; }

inline nobion::SimStep raw(nobion::Bytes tx, nobion::Bytes rx) { return nobion::SimStep{std::move(tx), std::move(rx), {}}; }

inline toml::table table_of(std::string_view text) {
  auto r = toml::parse(text, std::string_view("spectrometer.toml"));
  EXPECT_TRUE(r) << r.error().description();
  return r ? std::move(r).table() : toml::table{};
}

inline void expect_verified(const nobion::SimTransport& sim) {
  auto v = sim.verify();
  EXPECT_TRUE(v) << (v ? "" : nobion::to_string(v.error()));
}

}  // namespace legacy_test
