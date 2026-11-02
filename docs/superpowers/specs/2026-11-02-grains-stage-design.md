# Grains stage control: design

Status: agreed, 2026-11-02. x,y by key steps only for first version. Builds on `2026-10-30-grains-design.md`; replaces
its "Stage or z drivers and motion" out-of-scope line. Legacy reference: `pychron/loading/loading_manager.py`
(`use_stage` path), `pychron/hardware/zaber`, `pychron/hardware/jss57.py`.

## 1. Purpose

Operator moves sample from `grains` window instead of by hand. x,y on Zaber
stage today, z on JMC integrated stepper; other layouts must need only
`grains.toml`.
Motion is convenience: labels never depend on it.

Risk that shapes design: z drives objective toward slide in oil. Bad z move
breaks objective or sample. No motion without operator action; every move
bounded.

## 2. Rules that must hold

- `GrainPipeline` still has no z, no clock, no stage input (`libs/grains/AGENTS.md`).
  Sign comes from reference grain only, even under motorised z. No stage
  position in `process`, `tracks.jsonl` or replay.
- App with no stage, or failed stage, labels as before. Stage never blocks
  frame path or GUI thread.
- No motion at start, at connect, or from config. Every move comes from one
  operator action. Homing too.
- Target outside `min_mm`..`max_mm` never reaches driver.
- Axis not homed takes no move. Home only.
- No hold-to-run. One key press is one bounded step: frozen app, dropped link or
  stuck key cannot run axis on.
- Stop always available, overtakes queued work, sent on Escape, window close,
  app exit.
- Failed axis stays failed until operator reconnects. No automatic retry of a
  move.

## 3. Structure

- `libs/stage` (new; depends on `core`, `devices`; no Qt, no OpenCV, no vision):
  `IAxis`, `GuardedAxis`, `Jss57Axis`, `SimAxis`, `StageRunner`, `open_stage`,
  `load_stage_config`.
- `libs/stage/src/zaber_axis.cpp`: Zaber adapter, only file that sees Zaber
  headers. `libs/stage/src/zaber_stub.cpp` without it.
- `libs/devices`: `Jss57` driver (section 6). Owns its wire, so `libs/devices`
  rules apply.
- `apps/grains`: stage panel, keys. Links `pychron::stage`.
- `libs/grains` keeps depending on `core` and `vision` only.
- Zaber adapter not in `libs/devices`: drivers there own wire through
  `Transport`, `Link`, `Device::call`; Zaber library owns its serial port. Not
  `extraction/interfaces.hpp`
  `Axis`: that interface moves to named tray positions.

### Config moves

`StageController`, `StageAxis`, `StageConfig` and their parsing move from
`libs/grains` to `libs/stage`. Table stays in `grains.toml`:
`stage::load_stage_config(path)` reads only `[stage]`. `grains::load_config`
allows key `stage`, does not read it. `apps/grains` calls both. Grains spec
section 10 and `libs/grains/AGENTS.md` follow.

New keys, `[stage.axes.x|y|z]`:

- `jog_mm`: fine step, default `0.1` (legacy `relative_move` step).
- `coarse_mm`: coarse step, default `1.0`.

Both above 0 and at most `max_mm - min_mm`.

## 4. Interfaces

```cpp
namespace pychron::stage {

enum class AxisId { X, Y, Z };

struct AxisState {
  double position_mm = 0;  // operator's frame: sign applied
  bool busy = false;
  bool homed = false;
};

// One motor, raw frame. Starts motion and returns: does not wait for it.
// Called from one thread only (StageRunner's).
class IAxis {
 public:
  virtual ~IAxis() = default;
  [[nodiscard]] virtual Result<void> move_to(double raw_mm) = 0;
  [[nodiscard]] virtual Result<void> home() = 0;
  [[nodiscard]] virtual Result<void> stop() = 0;
  [[nodiscard]] virtual Result<AxisState> state() = 0;  // raw frame
};

// Applies StageAxis: sign, limits, homed gate. Backend-independent.
class GuardedAxis {
 public:
  GuardedAxis(std::unique_ptr<IAxis>, StageAxis);
  [[nodiscard]] Result<void> move_to(double mm);  // outside limits: refused
  [[nodiscard]] Result<void> step(double delta_mm);  // clamped to limits
  [[nodiscard]] Result<void> home();
  [[nodiscard]] Result<void> stop();
  [[nodiscard]] Result<AxisState> state();
};

// The axes config names, and the connections they share. Axis left out: empty.
struct Stage {
  std::vector<std::shared_ptr<void>> connections;  // first: released after the axes
  std::optional<GuardedAxis> x, y, z;
};

// Connects every controller named by an axis. Connecting moves nothing.
// Unknown or unbuilt kind: Config error naming kind and axis.
[[nodiscard]] Result<Stage> open_stage(const StageConfig&);

}  // namespace pychron::stage
```

`StageRunner`: one worker thread, owns `Stage`. Commands from GUI through
mailbox: at most one pending motion per axis, newer replaces older; `stop`
clears pending and runs next. Publishes `AxisState` for each axis about 10 times
a second, plus last error. All Zaber calls on this thread.

Sign as legacy: `sign = -1`, position = `max_mm` minus raw. So raw =
`max_mm` minus position.

`step` base is last commanded target while busy, else read position: repeated
key presses add up, no drift from mid-move reads. Wire move always absolute:
sent twice, it does no more (`libs/devices` marking rule: `Safe`). Zaber
relative move not used.

## 5. Zaber adapter

Zaber Motion Library, C++ (`zaber_motion` in legacy pychron).

- `PYCHRON_ZABER=AUTO|ON|OFF`, default `AUTO`: same pattern as
  `PYCHRON_PUBLISH`. Headers in `zaber_axis.cpp` only. No vcpkg port found;
  installer or source build, found by CMake.
- One `Connection` per `[stage.controllers.<name>]`, shared by its axes; opened
  on `port`. `device` and `axis` locate motor (`getDevice`, `getAxis`).
- Moves with wait-until-idle false; mm through library units.
- Library throws (`MotionLibException`). Adapter catches every exception at its
  boundary, returns `Result` (root rule: `libs/` does not throw). Message kept
  for status line.
- Library calls checked against C++ API reference at plan time: only Python
  reference and guides read so far.
- Device database: unit conversion needs it. Legacy enables local store
  (`Library.enable_device_db_store`). Lab PC may be offline: adapter enables
  store, first connect documented.
- Requires ASCII protocol. Legacy loader uses `stage_controller_klass =
  "ZaberMotion"`, ASCII library, so rig speaks it. Binary-only device fails at
  connect with this named.
- Legacy `open()` with `integrated_axes` true reads `self._device`, never set: only
  `false` works there, so rig is x and y as separate device addresses. Example
  config has `device = 1`, `2`; bench confirms.
- Legacy `relative_move` hard-codes x inverted, apart from axis `sign`. Here only
  `sign`, set on bench.

## 6. z adapter

Legacy `STP_MTRD` (`hardware/jss57.py`, device `focusmotor`), `kind = "jss57"`.
Assumed JMC JSS57: name only; no JMC manual or `focusmotor.cfg` found. Serial,
one ASCII command per line, each prefixed by `address` (legacy `"1"`). Driver
`libs/devices` `Jss57`, through `Device::call`; `Jss57Axis` adapts it to
`IAxis`, converts by `steps_per_mm`. Table in header, rows in
`test_repeat_tables.cpp`.

| Use | Commands | Repeat |
|---|---|---|
| Read position | `IP` (reply `...=<hex>`) | `Safe` |
| Read status | `SC` (reply `...=<hex>`) | `Safe` |
| Setup at connect, no motion | `AC`, `DE`, `VE`, `DL3` | `Safe` |
| Absolute move | `DI<steps>`, `FP`: one transaction | `Safe` |
| Stop | `SK` | `Safe` |
| Home | `DI-1`, `SH1H`, wait, `EP0`, `SP0` | `Unsafe` |

Options in `[stage.controllers.<name>.options]`: `address` (`"1"`),
`steps_per_mm` (required; legacy 964), `velocity` (0.25), `acceleration` (2.5),
`home_position` (legacy 12, loader value), `home_timeout_s` (30), `baudrate`
(9600, legacy value not found).

Departs from legacy, each on purpose:

- `FL` relative move not used.
- Moving: bit test `status & 0x0010`. Legacy `value > 16` misses `0x0010` alone,
  counts any higher bit as moving.
- Position: 32-bit two's complement. Legacy reads below-zero as huge.
- Home timeout: axis `failed`, no `EP0`, `SP0`. Legacy goes on, zeroes wherever
  motor stopped.
- Stop exists. Legacy has none. `SK` from SCL command family these commands
  belong to, unconfirmed on this motor: bench gate, before any other z motion.
- After home, moves to `home_position`, as legacy; must be inside limits. Part of
  same operator action.
- `homed` true only after home in this session: `EP0` is set by it, no memory of
  it across restart.

## 7. Screen

- Stage panel, only when config has an axis: per axis position in mm, state
  (`not homed`, `moving`, `idle`, `failed`), Home button.
- Keys: arrows x,y; Page Up / Page Down z; Shift for `coarse_mm`; Escape stops
  all. Keys act only when window has focus. Key auto-repeat ignored: one press,
  one step.
- Home button confirms in dialog first. z dialog says objective may travel.
- Reconnect button when any axis failed.
- Stage errors in existing notice line. Labels untouched.

## 8. Effect on labels

x,y move shifts whole image. Existing global-shift check drops pass (grains
spec section 4). Large move loses tracks and reference: reference reads `Lost`,
operator re-clicks (grains spec section 5). No pipeline change. Tests pin that
step moves leave this behaviour as is, on `GrainSynth`.

Moving x,y during z sweep costs passes. Not prevented.

z shifts image too. Legacy loader compensates: canvas offset
`pxpermm / focus_scalar` per z unit (`focus_scalar` 15.5, `use_image_shift`):
at legacy default 55 px/mm, 3.5 px per unit, 0.9 px per 0.25 step. Grains runs
on loader rig, so shift happens under manual or motor z. `max_shift_px` 8 compares
consecutive frames, so sweep will not trip it. Risk is drift across segmenter
window and fringe ring: 2 units of z in 2 s moves grain about 7 px against
outline and ring. Grains validation gate (grains spec section 12) measures it on
first recorded sweep. No compensation here: it would put z into pipeline.

## 9. Failure

- Controller will not open: app runs, panel shows failed, error names port.
- Call fails or link drops: axis `failed`, pending cleared, no retry. Motion
  already started may finish: Zaber controller holds target. Bounded by step.
- `stop` fails: warning held on screen until reconnect, stating axis may be
  moving.
- Config bad: same as other `grains.toml` errors, app does not start.
- Exit: `stop` to all axes, then join worker.
- z home timeout, or `SC` shows no end of motion: axis `failed` (section 6).

## 10. Threads and time

`StageRunner` real time, not on simulated path, no `Clock::Participant`, as
`GrainRunner`: add to `2026-10-06-virtual-clock-design.md` section 4.5. Run
under `-DPYCHRON_SANITIZE=thread` before landing. Destructor waits for worker.

## 11. Testing

`SimAxis` (`kind = "sim"`): moves at fixed speed on injected clock, used in
tests and `--demo` (readout only: `GrainSynth` scene does not pan).

Rules with a test each:

- `sign = -1`: position and raw map both ways.
- Target outside limits: `IAxis` sees no call.
- Step past limit clamps to limit.
- Not homed: `move_to`, `step` refused, `home` allowed.
- Connecting, opening, loading config: no motion call.
- Repeated steps while busy add from last target.
- Newer pending motion replaces older.
- `stop` overtakes pending; pending cleared.
- After failure no call until reconnect.
- Unknown kind, or `zaber` in stub build: Config error naming it.
- `Jss57`: exact bytes of each command, on sim transport; `RepeatTable.Jss57`.
- `Jss57`: status `0x0010` alone reads moving; negative `IP` reads negative;
  home timeout sends no `EP0`, `SP0`.
- GUI: auto-repeat gives no step; no step without focus; Escape stops; close
  stops.
- With stage failed, frames still processed, labels same.

Not covered by CI: `zaber_axis.cpp`. As Arrow, stub on most jobs; compiled only
where library installed. Bench gate, before use on sample: on real stage, each
axis home, limits, sign, step size, Escape mid-move, cable pulled mid-move, with
objective raised clear of slide. z first, `SK` before all else. Measure
`steps_per_mm`: legacy `stepsperdata` 964 is per "data" unit, limits 0 to 52 and
`home_position` 12 in same unit; nothing in legacy says unit is mm.

## 12. Out of scope

Click-to-centre (needs px/mm calibration). Grain-by-grain tour. Tray or stage
maps. Autofocus. Motorised z sweep (legacy loader steps too). Zaber speed and
acceleration settings. Stage position in recordings. `elctl` commands.
Scripting. Binary Zaber protocol. Packaging.

## Open questions

None.
