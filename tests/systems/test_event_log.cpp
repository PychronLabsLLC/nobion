#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "log_capture.hpp"
#include "nobion/core/events.hpp"
#include "nobion/core/signal_bus.hpp"
#include "nobion/devices/spectrometer/detectors.hpp"
#include "nobion/systems/event_log.hpp"
#include "nobion/systems/jobs/job.hpp"
#include "nobion/systems/jobs/peak_center.hpp"
#include "nobion/systems/spectrometer/acquisition.hpp"
#include "nobion/systems/spectrometer/scan_service.hpp"
#include "nobion/systems/spectrometer/spectrometer.hpp"

namespace nobion::systems {
namespace {

using nobion::testing::LogCapture;
using Lines = std::vector<std::string>;

// Every struct libs/systems publishes on a SignalBus (jobs/job.hpp,
// jobs/peak_center.hpp, spectrometer/acquisition.hpp, spectrometer.hpp,
// scan_service.hpp; DetectorState is declared in libs/devices and published
// by the Spectrometer; DeviceHealthChanged is declared in libs/core and
// published by every Device a line makes). Kept by hand: a new event fails EveryEventClassified
// until systems_event_classes() has it.
const std::set<std::string_view> kSystemsEvents{
    "JobStarted",       "JobProgress",       "JobFinished",   "PeakCenterTryStarted", "PeakCenterPoint",
    "PeakCenterDone",   "IntensityReading",  "DetectorState", "MagnetMoved",          "FieldTableChanged",
    "ScanStatus",       "DeviceHealthChanged",
};

TEST(SystemsEventLog, EveryEventClassified) {
  std::set<std::string_view> classified;
  for (const auto& c : systems_event_classes()) EXPECT_TRUE(classified.insert(c.name).second) << c.name << " twice";
  EXPECT_EQ(classified, kSystemsEvents);
}

// Spec 2026-11-06-field-diagnostics, 7: warn going bad, info coming back.
TEST(SystemsEventLog, FaultedIsAWarning) {
  LogCapture log;
  ASSERT_TRUE(log.ok());
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(DeviceHealthChanged{"ig1", "gauges", DeviceState::Degraded, DeviceState::Faulted, 3, ErrorKind::Timeout,
                                  "no reply", {}});
  bus.publish(DeviceHealthChanged{"ig1", "gauges", DeviceState::Unknown, DeviceState::Degraded, 1, ErrorKind::Timeout,
                                  "no reply", {}});
  bus.publish(
      DeviceHealthChanged{"ig1", "gauges", DeviceState::Faulted, DeviceState::Ok, 0, ErrorKind::Timeout, "", {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[warn] trace.DeviceHealthChanged: device=ig1, transport=gauges, from=degraded, to=faulted, "
                   "consecutive_failures=3, last_error=timeout: no reply",
                   "[warn] trace.DeviceHealthChanged: device=ig1, transport=gauges, from=unknown, to=degraded, "
                   "consecutive_failures=1, last_error=timeout: no reply",
                   "[info] trace.DeviceHealthChanged: device=ig1, transport=gauges, from=faulted, to=ok, "
                   "consecutive_failures=0, last_error=timeout"}));
}

TEST(SystemsEventLog, JobStartedAndFinished) {
  LogCapture log;
  ASSERT_TRUE(log.ok());
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(jobs::JobStarted{7, "sweep", {}});
  jobs::Job job;
  job.id = 7;
  job.kind = "sweep";
  job.state = jobs::JobState::Failed;
  job.error = Error{ErrorKind::Cancelled, "stopped", "", ""};
  bus.publish(jobs::JobFinished{job});
  job.state = jobs::JobState::Succeeded;
  job.error.reset();
  bus.publish(jobs::JobFinished{job});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.JobStarted: id=7, kind=sweep",
                                      "[debug] trace.JobFinished: id=7, kind=sweep, state=failed, error=cancelled: stopped",
                                      "[debug] trace.JobFinished: id=7, kind=sweep, state=succeeded"}));
}

TEST(SystemsEventLog, PeakCenterTryStarted) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(jobs::PeakCenterTryStarted{"H1", "Ar40", {"L1", "CDD"}, 0, 2, 4.9, 5.1, 5.0});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.PeakCenterTryStarted: detector=H1, isotope=Ar40, additional=[L1,CDD], try_index=0, "
                   "n_tries=2, start=4.9, end=5.1, initial_center=5"}));
}

TEST(SystemsEventLog, PeakCenterDone) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  jobs::PeakCenterResult r;
  r.ok = true;
  r.message = "centered";
  r.detector = "H1";
  r.isotope = "Ar40";
  r.initial_center = 5.0;
  r.center = 5.002;
  r.table_value = 5.001;
  r.table_updated = true;
  r.tries.resize(2);
  r.notes = {"field table not saved: disk full"};
  bus.publish(jobs::PeakCenterDone{r});
  r.ok = false;
  r.message = "no peak";
  r.center.reset();
  r.table_value.reset();
  r.table_updated = false;
  r.tries.resize(1);
  r.notes.clear();
  bus.publish(jobs::PeakCenterDone{r});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.PeakCenterDone: ok=true, message=centered, detector=H1, isotope=Ar40, "
                   "initial_center=5, center=5.002, table_value=5.001, table_updated=true, tries=2, "
                   "notes=[field table not saved: disk full]",
                   "[debug] trace.PeakCenterDone: ok=false, message=no peak, detector=H1, isotope=Ar40, "
                   "initial_center=5, table_updated=false, tries=1"}));
}

TEST(SystemsEventLog, DetectorState) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  spectrometer::DetectorState s;
  s.detector = "CDD";
  s.isotope = "Ar36";
  s.protected_ = true;
  s.deflection = 50;
  bus.publish(s);
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.DetectorState: detector=CDD, active=true, isotope=Ar36, protected=true, deflection=50"}));
}

TEST(SystemsEventLog, FieldTableChanged) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(spectrometer::FieldTableChanged{"argon", "3"});
  EXPECT_EQ(log.trace_lines(), (Lines{"[debug] trace.FieldTableChanged: name=argon, version=3"}));
}

TEST(SystemsEventLog, ScanStatusWarnsOnError) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  bus.publish(spectrometer::ScanStatus{"argus", true, false, std::chrono::seconds(1), "", {}});
  bus.publish(spectrometer::ScanStatus{"argus", false, false, std::chrono::seconds(1), "link down", {}});
  EXPECT_EQ(log.trace_lines(),
            (Lines{"[debug] trace.ScanStatus: spectrometer=argus, running=true, paused=false, integration=1s, error=",
                   "[warn] trace.ScanStatus: spectrometer=argus, running=false, paused=false, integration=1s, "
                   "error=link down"}));
}

TEST(SystemsEventLog, NeverLoggedEventsWriteNothing) {
  LogCapture log;
  SignalBus bus;
  const SystemsEventRecorder recorder(bus);
  const auto before = log.lines();
  bus.publish(jobs::JobProgress{1, "sweep", {}});
  bus.publish(jobs::PeakCenterPoint{0, {}});
  bus.publish(spectrometer::IntensityReading{});
  bus.publish(spectrometer::MagnetMoved{});
  EXPECT_EQ(log.lines(), before);
}

}  // namespace
}  // namespace nobion::systems
