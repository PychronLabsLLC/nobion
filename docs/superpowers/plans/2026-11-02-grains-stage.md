# Grains Stage Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the operator home and step the x, y and z axes of the loader rig from the `grains` window, with Zaber x,y and a JSS57 z, and no effect on labels.

**Architecture:** New `libs/stage` holds the axis interface, the guard that applies limits and sign, a simulated axis, the Zaber adapter, and a worker thread that owns all hardware calls. The JSS57 wire driver goes in `libs/devices` under that library's repeat rules. `apps/grains` adds a panel and keyboard shortcuts; nothing reaches `GrainPipeline`.

**Tech Stack:** C++20, Qt 6 Widgets (app only), Zaber Motion Library C++ (optional), GoogleTest, Qt Test.

**Spec:** `docs/superpowers/specs/2026-11-02-grains-stage-design.md`

## Global Constraints

- `GrainPipeline`, `tracks.jsonl` and replay gain no stage or z input.
- `libs/grains` keeps depending on `core` and `vision` only.
- `libs/stage` has no Qt, no OpenCV, no vision.
- Zaber headers appear only in `libs/stage/src/zaber_axis.cpp`. Every exception from the library is caught there and returned as `Result`.
- No motion at start, connect or config load. Every move comes from one operator action.
- A target outside `min_mm`..`max_mm` never reaches an `IAxis`.
- An axis not homed accepts only `home`.
- One key press is one bounded step. Auto-repeat gives no step.
- Wire moves are absolute. No Zaber relative move, no JSS57 `FL`.
- A failed axis stays failed until reconnect. No move is retried.
- All `IAxis` calls happen on the `StageRunner` thread.
- `StageRunner` stays on real time (no `Clock::Participant`).
- Commit messages: Conventional Commits with the component as scope; end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- After C++ changes: `python3 tools/quality_check.py` exits 0, `python3 tools/compiler_sweep.py --changed` reports 0 warnings. cppcheck treats the word `slots` as a Qt macro: do not name a variable that.

## Review Focus

- Escape during z home: `SK` is sent, home ends, axis reads not homed, and neither `EP0` nor `SP0` is ever sent. Test in Task 4.
- Reconnect after a failure with a step still pending: the pending step is gone, and a `jss57` axis reads not homed. Test in Task 7.
- Position read fails while a step is requested: no move is sent and the axis is failed. Test in Task 2.
- Bad z options (`steps_per_mm = 0`, text in a number, `home_position` outside limits): Config error from `open_stage` before any byte is sent. Test in Task 5.
- Window closed while the worker is still opening a dead port: destructor returns once the open times out, with no crash. Test in Task 7.

---

### Task 1: `libs/stage` and the config move

**Files:**
- Create: `libs/stage/CMakeLists.txt`, `libs/stage/include/pychron/stage/config.hpp`, `libs/stage/src/config.cpp`, `libs/stage/AGENTS.md`, `libs/stage/CLAUDE.md`, `tests/stage/CMakeLists.txt`, `tests/stage/test_config.cpp`
- Modify: `CMakeLists.txt` (add `stage` after `devices` in `PYCHRON_LIBS`), `AGENTS.md` (table row), `libs/grains/include/pychron/grains/config.hpp`, `libs/grains/src/config.cpp`, `tests/grains/test_config.cpp`, `tests/grains/CMakeLists.txt`, `apps/grains/grains.toml.example`

**Interfaces:**
- Produces: `pychron::stage::StageController`, `StageAxis`, `StageConfig` (moved unchanged, with `double jog_mm = 0.1; double coarse_mm = 1.0;` added to `StageAxis`); `[[nodiscard]] Result<StageConfig> load_stage_config(const std::filesystem::path&)`; `[[nodiscard]] bool any_axis(const StageConfig&)`.

- [ ] **Step 1: Move the stage tests to `tests/stage/test_config.cpp`**

Move every `Stage*`-related test from `tests/grains/test_config.cpp`, and the `PYCHRON_GRAINS_EXAMPLE_CONFIG` definition, and call `load_stage_config`. Keep `kCamera` out: the file may hold only `[stage]`, and a `[camera]` table beside it is ignored. Add:

```cpp
TEST_F(StageConfigTest, AnotherTablesKeysAreNotItsBusiness) {
  const auto cfg = load_stage_config(write("[camera]\nbackend = \"opencv\"\n[pipeline]\nmin_passes = 3\n"));
  ASSERT_TRUE(cfg.has_value());
  EXPECT_FALSE(any_axis(*cfg));
}

TEST_F(StageConfigTest, StepsDefaultAndCanBeSet) {
  const auto cfg = load_stage_config(write(std::string(kController) + kAxisX + "jog_mm = 0.25\n"));
  ASSERT_TRUE(cfg.has_value()) << cfg.error().what;
  EXPECT_DOUBLE_EQ(cfg->x->jog_mm, 0.25);
  EXPECT_DOUBLE_EQ(cfg->x->coarse_mm, 1.0);
}

TEST_F(StageConfigTest, AStepLongerThanTheTravelIsAnError) {
  const auto cfg = load_stage_config(write(std::string(kController) + kAxisX + "coarse_mm = 51\n"));
  ASSERT_FALSE(cfg.has_value());
  EXPECT_NE(cfg.error().what.find("coarse_mm"), std::string::npos);
}
```

In `tests/grains/test_config.cpp` add one test: a file with `[stage]` holding an unknown key loads, because `grains::load_config` does not read that table.

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --preset dev && cmake --build --preset dev --target pychron_stage_tests`
Expected: fails, `pychron/stage/config.hpp` not found.

- [ ] **Step 3: Implement**

Move the types and `read_controller`, `read_axis`, `read_stage` to `libs/stage`, and copy into its anonymous namespace the helpers they use (`unknown_key`, `number`, `whole`, `set_positive_int`, `read_options`). `libs/grains` and `libs/stage` share no helper header, since neither may depend on the other. `libs/grains/src/config.cpp` keeps its own copies. `grains::load_config` accepts key `stage` and skips it. `jog_mm`, `coarse_mm`: above 0 and at most `max_mm - min_mm`, checked after both limits are read. `libs/stage` links `pychron::core` publicly (toml++ comes with it). `libs/stage/AGENTS.md` holds the rules of spec section 2, compressed; `CLAUDE.md` holds `@AGENTS.md`; root table gets row `libs/stage, tests/stage, stage panel of apps/grains`. Example config: add `jog_mm = 0.25` to z.

- [ ] **Step 4: Run tests**

Run: `cmake --build --preset dev --target pychron_stage_tests pychron_grains_tests && build/dev/tests/stage/pychron_stage_tests && build/dev/tests/grains/pychron_grains_tests --gtest_filter='Config.*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add libs/stage tests/stage CMakeLists.txt AGENTS.md libs/grains tests/grains apps/grains/grains.toml.example docs
git commit -m "feat(stage): a library for the stage and its config"
```

### Task 2: `IAxis`, `GuardedAxis`, `SimAxis`

**Files:**
- Create: `libs/stage/include/pychron/stage/axis.hpp`, `libs/stage/src/axis.cpp`, `libs/stage/include/pychron/stage/sim_axis.hpp`, `libs/stage/src/sim_axis.cpp`, `tests/stage/fake_axis.hpp`, `tests/stage/test_axis.cpp`

**Interfaces:**
- Consumes: `StageAxis` (Task 1).
- Produces: `AxisId`, `AxisState`, `IAxis`, `GuardedAxis` exactly as spec section 4. `class SimAxis final : public IAxis { SimAxis(const Clock&, double speed_mm_s); }`. `FakeAxis` (test only) records calls in `std::vector<std::string>` and can be told to fail its next call.

- [ ] **Step 1: Write the failing tests** in `tests/stage/test_axis.cpp`, on `FakeAxis`, one each:

```cpp
TEST(GuardedAxis, SignMapsBothWays);            // sign -1, max 50: move_to(10) sends raw 40; raw 40 reads 10
TEST(GuardedAxis, TargetOutsideLimitsSendsNothing);
TEST(GuardedAxis, StepPastLimitClampsToLimit);  // position 49.95, step 0.1, max 50: raw target for 50
TEST(GuardedAxis, NotHomedRefusesMoveAndStepAllowsHome);
TEST(GuardedAxis, StepsWhileBusyAddFromLastTarget);  // step 1 busy, step 1: second target is start + 2
TEST(GuardedAxis, FailedPositionReadSendsNoMove);    // state() fails: step returns error, no move_to call
TEST(SimAxis, MovesAtItsSpeedOnItsClock);            // ManualClock: 2 mm at 1 mm/s, busy at 1 s, idle at 2 s
TEST(SimAxis, StopHoldsWhereItIs);
TEST(SimAxis, HomeGoesToZeroAndSetsHomed);
```

Each name states its assertion; values in the comments are exact.

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --build --preset dev --target pychron_stage_tests`
Expected: fails, `pychron/stage/axis.hpp` not found.

- [ ] **Step 3: Implement** as spec section 4. `GuardedAxis` keeps `std::optional<double> last_target_` (operator frame), cleared when `state()` reads not busy. `SimAxis` computes position from clock time and linear motion, no thread.

- [ ] **Step 4: Run tests**

Run: `build/dev/tests/stage/pychron_stage_tests --gtest_filter='GuardedAxis.*:SimAxis.*'` after building.
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add libs/stage tests/stage
git commit -m "feat(stage): an axis with limits, sign and a homed gate"
```

### Task 3: JSS57 codec and driver

**Files:**
- Create: `libs/codecs/include/pychron/codecs/jss57.hpp`, `libs/codecs/src/jss57.cpp`, `libs/devices/include/pychron/devices/jss57.hpp`, `libs/devices/src/jss57.cpp`, `libs/devices/include/pychron/devices/jss57_sim.hpp`, `libs/devices/src/jss57_sim.cpp`, `tests/codecs/test_jss57.cpp`, `tests/devices/test_jss57.cpp`
- Modify: `tests/devices/test_repeat_tables.cpp`

**Interfaces:**
- Produces, in `pychron::codec::jss57`: `Bytes request(std::string_view address, std::string_view command)` (`address + command + "\r"`); `Result<std::int32_t> parse_position(std::string_view reply)` (hex after `=`, 32-bit two's complement); `Result<std::uint32_t> parse_status(std::string_view reply)`; `inline constexpr std::uint32_t kStatusMoving = 0x0010`.
- Produces: `class Jss57 final : public Device` with `Jss57(std::string name, Transport&, std::string address, DeviceOptions = {})` and `Result<void> configure(double velocity, double acceleration)`, `Result<void> move_to_steps(std::int32_t)`, `Result<void> seek_home()`, `Result<void> zero()`, `Result<void> stop()`, `Result<std::int32_t> position_steps()`, `Result<bool> moving()`. Header carries the Repeat table of spec section 6 as a comment.
- `Jss57Sim` with `SimTransport::Hook hook()`, modelled on `ProxrBoardSim`: answers the commands, keeps a list of received commands.

- [ ] **Step 1: Write the failing tests**

```cpp
TEST(Jss57Codec, RequestIsAddressCommandCarriageReturn);  // "1","IP" -> "1IP\r"
TEST(Jss57Codec, PositionIsTwosComplement);               // "1IP=FFFFFFFF" -> -1; "1IP=000003C4" -> 964
TEST(Jss57Codec, ReplyWithoutEqualsIsAProtocolError);
TEST(Jss57Driver, ConfigureSendsTheLegacySetupAndNoMotion);  // AC2.5, DE2.5, VE0.25, DL3; no DI, FP, SH
TEST(Jss57Driver, MoveIsDistanceThenFeedToPosition);      // DI964 then FP, one transaction
TEST(Jss57Driver, SeekHomeIsDirectionThenSeek);           // DI-1 then SH1H
TEST(Jss57Driver, ZeroSetsEncoderThenPosition);           // EP0 then SP0
TEST(Jss57Driver, StatusMovingAloneReadsMoving);          // 0x0010 -> true; 0x0001 -> false; 0x0100 -> false
```

And `TEST_F(RepeatTable, Jss57)` following `ChromiumLaser`: rows Safe for `IP`, `SC`, `AC`, `VE`, `DL3`, `DI`+`FP`, `SK`; Unsafe for `SH1H`, `EP0`, `SP0`.

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --build --preset dev --target pychron_codecs_tests pychron_devices_tests`
Expected: fails, headers not found.

- [ ] **Step 3: Implement** through `Device::call` with the marked `Repeat`; `move_to_steps` is one `transact` under the driver's `ClockMutex`. Not registered with `REGISTER_DRIVER`: `open_stage` builds it directly.

- [ ] **Step 4: Run tests**

Run: `build/dev/tests/codecs/pychron_codecs_tests --gtest_filter='Jss57*' && build/dev/tests/devices/pychron_devices_tests --gtest_filter='Jss57*:RepeatTable.Jss57'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add libs/codecs libs/devices tests/codecs tests/devices
git commit -m "feat(devices): a driver for the JSS57 focus motor"
```

### Task 4: `Jss57Axis`

**Files:**
- Create: `libs/stage/include/pychron/stage/jss57_axis.hpp`, `libs/stage/src/jss57_axis.cpp`, `tests/stage/test_jss57_axis.cpp`
- Modify: `libs/stage/CMakeLists.txt` (link `pychron::devices`), `tests/stage/CMakeLists.txt`

**Interfaces:**
- Consumes: `IAxis`, `Jss57`, `Jss57Sim`.
- Produces: `struct Jss57Options { double steps_per_mm; double home_position_mm; double home_timeout_s; };` `class Jss57Axis final : public IAxis { Jss57Axis(Jss57&, Jss57Options, const Clock&); }`.

Home does not block, so `stop` can overtake it. It is a state machine advanced by `state()`, which the runner calls about 10 times a second: `Idle` → `home()` sends `seek_home` → `Seeking`. In `Seeking`, `state()` reads `moving()`: past deadline, returns an error and goes `Idle`, no zero; not moving, `zero()`, set `homed_`, `move_to_steps(home_position)`, `Idle`. `stop()` sends `SK` and from `Seeking` goes `Idle`, `homed_` stays false. Report `busy` true while `Seeking`.

- [ ] **Step 1: Write the failing tests**, `ManualClock`, `Jss57Sim`:

```cpp
TEST(Jss57Axis, ConvertsMillimetresToSteps);       // 964 steps/mm: move_to(2.0) sends DI1928
TEST(Jss57Axis, StartsNotHomed);
TEST(Jss57Axis, HomeReturnsAtOnceAndFinishesThroughState);  // SK reaches sim while seeking
TEST(Jss57Axis, HomeZeroesThenGoesToHomePosition);  // order: SH1H, EP0, SP0, DI..., FP; homed true
TEST(Jss57Axis, HomeTimeoutFailsAndNeverZeroes);    // 31 s of ManualClock: state() errors, no EP0, no SP0
TEST(Jss57Axis, StopDuringHomeEndsItNotHomed);      // SK sent, no EP0, no SP0, homed false
```

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --build --preset dev --target pychron_stage_tests`
Expected: fails, `jss57_axis.hpp` not found.

- [ ] **Step 3: Implement** the state machine above.

- [ ] **Step 4: Run tests**

Run: `build/dev/tests/stage/pychron_stage_tests --gtest_filter='Jss57Axis.*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add libs/stage tests/stage
git commit -m "feat(stage): the focus motor as an axis"
```

### Task 5: `open_stage` and the Zaber option

**Files:**
- Create: `libs/stage/include/pychron/stage/stage.hpp`, `libs/stage/src/stage.cpp`, `libs/stage/src/zaber_stub.cpp`, `libs/stage/include/pychron/stage/zaber_axis.hpp`, `tests/stage/test_open_stage.cpp`
- Modify: `cmake/PychronDependencies.cmake`, `libs/stage/CMakeLists.txt`

**Interfaces:**
- Consumes: `StageConfig`, `GuardedAxis`, `SimAxis`, `Jss57Axis`, `make_transport`.
- Produces: `struct Stage { std::vector<std::shared_ptr<void>> connections; std::optional<GuardedAxis> x, y, z; };` `[[nodiscard]] Result<Stage> open_stage(const StageConfig&, const Clock& clock);` `[[nodiscard]] Result<std::shared_ptr<void>> open_zaber(const StageController&);` `[[nodiscard]] Result<std::unique_ptr<IAxis>> make_zaber_axis(const std::shared_ptr<void>&, const StageAxis&);` (stub: Config error `"zaber: built without the Zaber Motion Library"`.)

Kinds: `sim`, `jss57`, `zaber`. JSS57 port is a `SerialParams` `TransportConfig`, `baud` 9600 unless `baudrate` is set; replies end in CR (`read_spec.hpp`). Options parsed with `std::from_chars`: `address` default `"1"`, `steps_per_mm` required above 0, `velocity` default 0.25, `acceleration` default 2.5, `home_position` required and inside the axis limits, `home_timeout_s` default 30, `speed_mm_s` for `sim` default 5.

- [ ] **Step 1: Write the failing tests** in `tests/stage/test_open_stage.cpp`:

```cpp
TEST(OpenStage, SimAxesOpenAndNothingMoves);
TEST(OpenStage, UnknownKindNamesKindAndAxis);
TEST(OpenStage, AxisLeftOutIsEmpty);
TEST(OpenStage, ZeroStepsPerMmIsAConfigError);
TEST(OpenStage, TextInANumberIsAConfigError);
TEST(OpenStage, HomePositionOutsideLimitsIsAConfigError);
TEST(OpenStage, ZaberInAStubBuildSaysSo);  // GTEST_SKIP when PYCHRON_ZABER_ENABLED
```

Option checks run before the transport is built, so these three send nothing.

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --build --preset dev --target pychron_stage_tests`
Expected: fails, `stage.hpp` not found.

- [ ] **Step 3: Implement.** Add `PYCHRON_ZABER` to `cmake/PychronDependencies.cmake` as `PYCHRON_PUBLISH` does (`AUTO|ON|OFF`, default `AUTO`, `PYCHRON_STAGE_ZABER_ENABLED`, `PYCHRON_STAGE_ZABER_LIBS`), finding library `zml` and header `zaber/motion/ascii.h` by `find_library`, `find_path`. `zaber_stub.cpp` compiles when disabled.

- [ ] **Step 4: Run tests**

Run: `build/dev/tests/stage/pychron_stage_tests --gtest_filter='OpenStage.*'`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add libs/stage tests/stage cmake
git commit -m "feat(stage): open the axes a config names"
```

### Task 6: Zaber adapter

**Requires Zaber Motion Library installed on the build machine** (its C++ install guide; `-DPYCHRON_ZABER=ON`). Installing is a download the developer approves first. If it is not approved, stop this task, leave the stub, and say so: code that has never compiled is not landed.

**Files:**
- Create: `libs/stage/src/zaber_axis.cpp`
- Modify: `libs/stage/CMakeLists.txt`

**Interfaces:**
- Produces: real `open_zaber`, `make_zaber_axis` of Task 5.

- [ ] **Step 1: Read the C++ `zaber::motion::ascii` reference** for `Connection::openSerialPort`, `Connection::getDevice`, `Device::getAxis`, `Axis::moveAbsolute`, `Axis::home`, `Axis::stop`, `Axis::isBusy`, `Axis::getPosition`, `Axis::isHomed`, and the device database store. Note any name that differs. The spec had only Python pages and guides.

- [ ] **Step 2: Implement.** One `Connection` in `open_zaber`, device database store enabled in a cache directory. Each `IAxis` call catches `MotionLibException` and any `std::exception`, returning `ErrorKind::Io` with its message. Moves pass wait-until-idle false and `Units::LENGTH_MILLIMETRES`. `home` too does not wait. A device that refuses ASCII: error text names the ASCII protocol.

- [ ] **Step 3: Build with the library**

Run: `cmake --preset dev -DPYCHRON_ZABER=ON && cmake --build --preset dev --target pychron_stage_tests && build/dev/tests/stage/pychron_stage_tests`
Expected: compiles, PASS (`ZaberInAStubBuildSaysSo` skipped). No stage attached: the adapter's motion is proved at the bench gate (spec section 11).

- [ ] **Step 4: Commit**

```bash
git add libs/stage
git commit -m "feat(stage): Zaber axes through the Zaber Motion Library"
```

### Task 7: `StageRunner`

**Files:**
- Create: `libs/stage/include/pychron/stage/runner.hpp`, `libs/stage/src/runner.cpp`, `tests/stage/test_runner.cpp`
- Modify: `docs/superpowers/specs/2026-10-06-virtual-clock-design.md` section 4.5 (add `StageRunner` beside `GrainRunner`)

**Interfaces:**
- Consumes: `Stage`, `AxisId`, `AxisState`.
- Produces:

```cpp
struct AxisReport {
  bool present = false;
  bool failed = false;
  AxisState state;
  std::string error;
};
struct StageReport {
  std::array<AxisReport, 3> axes;  // indexed by AxisId
  bool connected = false;
  std::string error;
};
class StageRunner {
 public:
  StageRunner(std::function<Result<Stage>()> open, std::function<void()> on_report);
  ~StageRunner();  // stops every axis, joins
  void step(AxisId, double delta_mm);
  void home(AxisId);
  void stop_all();
  void reconnect();
  [[nodiscard]] StageReport report() const;
};
```

`open` runs on the worker, so a dead port does not block the GUI. Worker loop: wait on `std::condition_variable` up to 100 ms; run `stop` if asked, then the pending motion per axis, then read `state()` of each, publish, call `on_report` outside the mutex. `step` replaces a pending motion on that axis. An axis error sets `failed`, clears its pending motion; a failed axis gets no call until `reconnect`, except `stop`. `reconnect` clears all pending and destroys `Stage` before calling `open` again.

- [ ] **Step 1: Write the failing tests** on `FakeAxis`; bounds are upper bounds of 5 s or more:

```cpp
TEST(Runner, OpeningMovesNothing);
TEST(Runner, ANewerPendingStepReplacesAnOlder);
TEST(Runner, StopOvertakesPendingAndClearsIt);
TEST(Runner, AfterAFailureNoCallUntilReconnect);
TEST(Runner, ReconnectClearsAPendingStep);
TEST(Runner, Jss57ReadsNotHomedAfterReconnect);  // through a Jss57Sim-backed open function
TEST(Runner, DestructorStopsEveryAxis);
TEST(Runner, DestroyedWhileOpenHangsReturnsWhenOpenDoes);  // open blocks 200 ms on a latch, no crash
TEST(Runner, FailedOpenReportsAndWaitsForReconnect);
```

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --build --preset dev --target pychron_stage_tests`
Expected: fails, `runner.hpp` not found.

- [ ] **Step 3: Implement** as above.

- [ ] **Step 4: Run tests, and under TSan**

Run: `build/dev/tests/stage/pychron_stage_tests --gtest_filter='Runner.*' --gtest_repeat=20`
Then configure `-B build/tsan -DPYCHRON_SANITIZE=thread` and run the same binary there.
Expected: PASS, no race reported.

- [ ] **Step 5: Commit**

```bash
git add libs/stage tests/stage docs
git commit -m "feat(stage): a worker that owns the hardware"
```

### Task 8: Panel, keys and wiring

**Files:**
- Create: `apps/grains/src/stage_panel.hpp`, `apps/grains/src/stage_panel.cpp`, `apps/grains/tests/test_stage_panel.hpp`, `apps/grains/tests/test_stage_panel.cpp`
- Modify: `apps/grains/CMakeLists.txt`, `apps/grains/tests/CMakeLists.txt`, `apps/grains/tests/test_main.cpp`, `apps/grains/src/grains_window.hpp`, `apps/grains/src/grains_window.cpp`, `apps/grains/src/main.cpp`, `docs/superpowers/specs/2026-10-30-grains-design.md`, `libs/grains/AGENTS.md`, `apps/grains/grains.toml.example`

**Interfaces:**
- Consumes: `StageRunner`, `StageReport`, `StageConfig`.
- Produces: `class StagePanel : public QWidget { StagePanel(StageRunner&, StageConfig, std::function<bool(stage::AxisId)> confirm_home, QWidget* parent = nullptr); void refresh(const stage::StageReport&); };` and `GrainsWindow::add_stage_panel(StagePanel*)`, which docks it and adds its shortcuts to the window.

Keys are `QShortcut`s with `setAutoRepeat(false)` and `Qt::WindowShortcut`: Left and Right x, Up and Down y, Page Up and Page Down z; with Shift, `coarse_mm` in place of `jog_mm`; Escape `stop_all`. Direction follows the key and `sign` is the bench's business. An axis left out has no shortcut. Default `confirm_home` is a `QMessageBox::question`; for z its text is "Home z? The objective may travel. Check it is clear of the slide." Panel state text: `not homed`, `moving`, `idle`, `failed`. The wake from `on_report` carries nothing, as `GrainsWindow`'s does. `main.cpp`: config path loads `load_stage_config`, and a `StageRunner` is made only when `any_axis`; it is declared before the window. `--demo`: three `SimAxis`, panel shown, readout only.

- [ ] **Step 1: Write the failing tests** in `test_stage_panel.cpp`, Qt Test, on a `FakeAxis` runner:

```cpp
void arrowStepsOneJog();                 // Right: one step of jog_mm on x
void shiftUsesCoarse();
void autoRepeatGivesNoStep();            // QKeyEvent with autorep true: no step
void noStepWithoutFocus();               // another window active: no step
void escapeStopsAll();
void homeAsksFirstAndDeclineSendsNothing();
void axisLeftOutHasNoRowAndNoKey();
void failedAxisShowsReconnect();
void closingTheWindowStops();
void framesStillProcessedWhenStageFailed();  // demo poll + failed open: reports keep coming, labels unchanged
```

- [ ] **Step 2: Build to confirm it fails**

Run: `cmake --build --preset dev-ui --target pychron_grains_app_tests`
Expected: fails, `stage_panel.hpp` not found.

- [ ] **Step 3: Implement** as above. In grains spec section 10 point `[stage]` at `libs/stage`; replace "Read and checked only" and "no driver" with a pointer to the stage spec. In `libs/grains/AGENTS.md` replace the `[stage]` line: no z or stage input to `GrainPipeline`, whoever moves the stage. In the example config, drop "Nothing moves yet: no driver reads this table."

- [ ] **Step 4: Run tests**

Run: `cmake --build --preset dev-ui --target pychron_grains_app_tests grains && QT_QPA_PLATFORM=offscreen build/dev-ui/apps/grains/tests/pychron_grains_app_tests`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add apps/grains libs/grains docs
git commit -m "feat(grains): step the stage from the window"
```

### Task 9: Whole-branch verification

**Files:** none new.

- [ ] **Step 1: Static analysis**

Run: `python3 tools/quality_check.py`, then `python3 tools/compiler_sweep.py --build-dir build/dev-ui`
Expected: 0 findings, 0 warnings. Note how many Qt GUI sources g++ could not check.

- [ ] **Step 2: Sanitizers**

Build with `-DPYCHRON_SANITIZE=address,undefined`, run `pychron_stage_tests`, and the `Jss57*` filters of the codecs and devices tests.
Expected: clean.

- [ ] **Step 3: Full suite**

Run: `cmake --build --preset dev-ui && ctest --preset dev-ui`
Expected: 100% pass.

- [ ] **Step 4: Confirm the pipeline stayed clean**

Run: `git diff origin/develop... --stat -- libs/grains/src/pipeline.cpp libs/grains/src/recording.cpp` compared with the same diff at this plan's first commit; and `grep -rn 'stage' libs/grains/src`.
Expected: no change from this plan; no match.

## Open questions

None.
