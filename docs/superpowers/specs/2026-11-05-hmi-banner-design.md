# HMI status banner, alarm strip, stale gauge mark

Date 2026-11-05. Status: built. Follows `2026-11-04-hmi-valves-design.md`
§2 (banner, strip, stale mark deferred there; gauge faceplates dropped:
gauge window and rest chart cover them).

## 1. Why

Main window shows line state only by reading canvas and docks. Nothing says
at a glance whether line is live or simulated, what executor is doing, how
many alarms stand, whether connections are healthy. A gauge that stops
answering keeps its last good value on canvas: dead gauge reads as good
pressure. ISA-101: top-of-screen status band, alarm summary always in view,
stale data marked.

## 2. Scope

In:

- `StatusBanner`: band across top of main window, above canvas.
- `AlarmStrip`: band under banner, shown only while alarms stand.
- Stale mark on canvas gauge chip.
- Alarm priority = severity: Critical above Warning. No new config.

Out:

- Rehearsal window: own toolbar, no live alarms or health. Unchanged.
- Gauge faceplates (dropped, see above).
- Alarm priority as config (ISA-18.2 levels), shelving, alarm cleared
  events from core (gauge leaving alarm is silent on bus today), acknowledge
  sent to core. Separate spec if wanted.
- Alarms dock behaviour: unchanged but for §5 accessors.
- Stale data anywhere but canvas chip (gauge window, heaters, cryo).

## 3. Layout

`MainWindow` central widget becomes wrapper (`QWidget`, `QVBoxLayout`,
zero margins, zero spacing): `StatusBanner`, `AlarmStrip`, `CanvasView`.
Band spans canvas column; docks sit beside it, not under it.
Menu bar unchanged (global bar on macOS: `setMenuWidget` not used). Dock
layouts unchanged: banner and strip are not docks, not saved in
`DockLayouts`.

## 4. `StatusBanner`

New `apps/nobion/src/status_banner.{hpp,cpp}` (also holds `AlarmStrip`). `QFrame`, one row, fixed
height 28 px, background `theme().chrome`, text `theme().on_chrome`.
Fields left to right, each a `QLabel` with object name for tests:

| Field | Object name | Shows | Source |
|---|---|---|---|
| Mode | `bannerMode` | "LIVE" (grey pill), "SIMULATED", "SIMULATED x50" (`--sim-speed`) | `MainWindow::set_line_mode(LineMode)`, from `main.cpp` (`sim`, `sim_speed`) |
| Line | `bannerLine` | line name | as window title |
| Executor | `bannerExecutor` | below | `ExperimentBridge` signals |
| Alarms | `bannerAlarms` | "No alarms", "2 critical · 1 warning" | `AlarmDock` (§5) |
| Connections | `bannerHealth` | "Connections ok (5)", "1 down · 1 degraded" | `HealthBar` (§6) |
| Clock | `bannerClock` | `HH:mm:ss` | `CoreBridge::wall_now()`, 1 s ticker |

```cpp
struct LineMode {
  bool simulated = false;
  double speed = 0;  // 0: real time; > 0: VirtualClock at N x
};
```

Mode chip: live a `muted_text` pill (grey: normal state, no alarm colour,
ISA-101); simulated an `accent_strong` pill, so a simulated session is
never mistaken for live.

Executor text by `ExecutorState` (`executor.hpp:72`):

| State | Text |
|---|---|
| no session (`set_experiment` not called) | "No experiment session" |
| session given while a queue runs (`ExperimentBridge::running()`) | "Running" until next `RunStarted` |
| `Idle` | "Idle" |
| `Preparing` | "Preparing queue" |
| `Running` | "Running <name>" (last `RunStarted::name`, `<identifier>-<aliquot><step>`; identifier when empty) |
| `Holding` | "Holding: <reason>" (`ExecutorHolding::reason`) |
| `StoppingAtBoundary` | "Stopping after <name>" |
| `Cancelling`, `Aborting` | "Cancelling", "Aborting" |
| `Finalizing` | "Finishing queue" |

Holding and Aborting in `warning_text`; rest `on_chrome`.

Alarms field: counts of standing alarms by severity, Critical and Warning
only (Info: dock only, §5). Critical count in `error_text` on
`error_bg`, warning in `warning_text`; "No alarms" `on_chrome`. Click:
show and raise Alarms dock.

Connections field: counts of `HealthBar` statuses. All Ok: "Connections ok
(N)". Else "<d> down · <g> degraded · <u> unknown", zero parts left out,
coloured by worst (`level_color`). Click: nothing (health chips are in the
status bar under it).

Clock: wall time of line's clock, so simulated session shows simulated
time. Tooltip names clock ("simulated, 50 times real time" or "real
time").

`StatusBanner` holds no line state: `MainWindow` pushes `set_mode`,
`set_line`, `set_session`, the executor events (`on_state`,
`on_run_started`, `on_holding`), `set_alarms(rows)`,
`set_health(HealthBar::Summary)`, and gives it a
`std::function<QDateTime()>` clock. Fields on dark band: normal state plain
text; state needing attention a pill (critical `error_bg`, warning and
holding `warning_bg`, simulated `accent_strong`, live `muted_text`).

## 5. Alarms: standing set and `AlarmStrip`

Standing alarm = row in `AlarmDock` (one per source, newest replaces,
removed only by acknowledge). Rule unchanged; banner and strip read it.
Ties of time broken by raise order (a counter per raise: same second, or
same simulated instant). A repeat of what a row says (same severity and
message) moves only its time and does not emit `changed()`.

`AlarmDock` gains:

```cpp
struct AlarmRow { std::string source; AlarmSeverity severity; std::string message; QDateTime time; };
std::vector<AlarmRow> rows() const;  // standing, Critical first, then newest first
signals: void changed();             // after add, acknowledge, acknowledge all
```

`AlarmStrip` (in `status_banner.{hpp,cpp}`): `QFrame`
under banner. Hidden while no Critical or Warning row stands. Shows up to
3 rows from `rows()` (Info left out), one line each:

```
CRITICAL  IG1  above alarm_high 1.0e-04 torr     13:41:07
WARNING   heater1  read failed: timeout          13:40:55
+2 more
```

Row background `error_bg` / `warning_bg`, text `on_error_bg` /
`warning_text`. "+N more" when more than 3. Click anywhere: show and raise
Alarms dock (acknowledge stays in dock: one place to acknowledge). Updated
on `AlarmDock::changed`.

Priority: Critical before Warning, then newest. Info never in strip or
banner count.

## 6. Health summary

`HealthBar` gains `HealthSummary summary() const` (counts per status:
Ok, Degraded, Down, Unknown) and `changed()` signal, emitted when a
chip's status changes (not on age ticks). Rule of status unchanged
(`health_bar.cpp:108-113`).

## 7. Stale gauge mark

- Stale: wired gauge whose newest `PressureSample` is older than
  `stale_after`, or that has had none for `stale_after` since view was
  built. Unwired gauge (canvas only) never stale.
- `stale_after` = 3 x `scan_interval_ms` (line-wide), rule of
  `LinePressureService` (`line_pressure_service.hpp`), one function for
  both: `systems::gauge_stale_after(const config::SystemSection&, int scans
  = kGaugeStaleScans)` (`kGaugeStaleScans` = 3) in
  `line_pressure_service.hpp`, `LinePressureService` switched to it. No
  new key.
- Sample age: `PressureSample::ts` is line `Clock::now()`; compared with
  `CoreBridge::now()` (same clock): simulated time in a simulation.
- Canvas: `CanvasView` over a bridge reads `gauge_stale_after` of
  `bridge_->config().system` once; `check_stale()` on a 1 s `QTimer`
  against `bridge_->now()`; `GaugeLabelItem::set_stale(bool, QString
  age)`. Read-only view (no bridge, no readings): `stale_after()` 0, marks
  nothing. A gauge never read since view was built: age text "no reading".
- Stale chip: value and dial in `theme().muted_text`, dashed chip outline,
  age appended: `IG1: 1.20e-08 torr · 45 s`. Age in s under 120 s, then
  min. Alarm red wins over stale grey? No: stale wins (value is not
  current, so its alarm is not either); tooltip says "No reading for 45 s;
  last value was in alarm" when both.
- Next sample clears it at once.
- Legend (`canvas_legend`) gains `gauge.stale` row: "No new reading for
  three scans (grey, dashed): the value is old, and so is any alarm it
  showed.".

## 8. Rules that must hold

- Banner, strip, stale mark read time only through line's clock
  (`CoreBridge::wall_now()`, `now()`), never `QDateTime::currentDateTime()`
  or `steady_clock` (root `AGENTS.md`, Time).
- Banner and strip send nothing to core; acknowledge only in dock.
- One staleness rule (`gauge_stale_after`), one standing-alarm rule
  (`AlarmDock`), one health rule (`HealthBar`): banner counts, never
  recomputes.
- Domain seam untouched.

## 9. Tests

- `tests/ui/test_status_banner.cpp` (new; also `AlarmDock::rows()` /
  `changed()`, `HealthBar::summary()` / `changed()`, strip): mode texts; executor text per
  state from `ExperimentBridge` signals; alarm counts follow dock add and
  acknowledge, Info not counted; health text from summary; clock reads
  injected clock; click on alarms raises dock.
- Strip (same file): hidden with none or Info only; Critical before
  Warning, newest first; 3 rows then "+N more"; click raises dock.
- `tests/ui/test_canvas_view.cpp`: gauge goes stale after 3 scans with no
  sample (simulated time, `VirtualClock` or injected `now`), clears on
  sample, unwired never stale, stale beats alarm colour.
- `tests/systems`: `gauge_stale_after` = 3 x scan interval;
  `LinePressureService` unchanged behaviour.
- `tests/ui/test_canvas_legend.cpp`: `gauge.stale` row.

User guide: `docs/user/02-extraction-line.md` (banner, strip, stale chip),
`01-getting-started.md` window sketch.

## Decisions

Agreed with developer 2026-11-05:

1. Stale limit: fixed rule, 3 scans, no key.
2. Live mode: explicit grey "LIVE" chip always.
3. Info alarms: dock only.
4. Strip click opens dock only; acknowledge stays in dock.
5. Stale beats alarm colour on chip.

## Open questions

None.
