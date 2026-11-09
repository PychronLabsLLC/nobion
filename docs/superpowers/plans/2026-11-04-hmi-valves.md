# HMI valves: implementation plan

Spec: `docs/superpowers/specs/2026-11-04-hmi-valves-design.md`.

Build with the `dev-ui` preset (the change is in `apps/nobion` and
`tests/ui`). UI suites are Qt Test executables named `pychron_ui_<file>`;
run one with `build/dev-ui/tests/ui/pychron_ui_test_canvas_view <case>`.

## Task 1: `valve_style` in canvas.toml

Files: `libs/systems/include/pychron/systems/canvas/canvas.hpp`,
`libs/systems/src/canvas/loader.cpp`, `tests/systems/test_canvas_loader.cpp`,
`configs/examples/canvas.toml`, `configs/examples/nmgrl/canvas.toml`,
`docs/user/07-configuration-reference.md`.

- `enum class ValveStyle { Hmi, Redgreen }`, `CanvasSection::valve_style`
  defaulting to `Hmi`, read with `read_enum` from `kValveStyles`.
- Tests: default is `Hmi`; both spellings parse; a bad value is a diagnostic
  with its line naming `hmi | redgreen`.

## Task 2: fill by style

Files: `apps/nobion/src/canvas_items.{hpp,cpp}`,
`apps/nobion/src/canvas_view.{hpp,cpp}`, `tests/ui/test_canvas_view.cpp`.

- `valve_fill(ValveState, canvas::ValveStyle)` replaces `valve_color`.
  `ValveItem::set_style()`; `fill_color()` reads it. Unknown is reported as
  `theme().base` (the hatch is drawn over it).
- `CanvasView::valve_style()` / `set_valve_style()`, initialised from the
  file, pushing the style to every valve.
- Tests: the existing inherit cases use `valve_fill(..., view style)`; a new
  case checks both styles per state and that inherit wins in both.

## Task 3: the glyph

Files: `canvas_items.{hpp,cpp}`, `canvas_view.cpp`, `test_canvas_view.cpp`.

- `ValveItem::set_pipe_sides(Sides)` (bit mask of left, right, top, bottom)
  set by `CanvasView::build` after the pipes are laid: for every pipe whose
  endpoints name the valve, the side its polyline leaves the valve by.
  From it: `vertical()` (pipes only top and/or bottom) and the caption side
  (below, else above, else right, else left: the first with no pipe).
- Paint: bow-tie (or circle for a switch), stem with dome or T bar, hatch
  for unknown, half fill and dashed outline while pending, padlock badge
  when locked, accent ring when marked or selected. The label is always a
  caption (`name_layout().caption` is true whenever the label is not empty).
- Removed: `kCornerRadius` body, `kWheelRadius`, `kCornerWheelRadius`,
  `wheel_rect()`, `kLockBorderWidth`, the face label.
- Tests replace `manualValvesWearAHandwheel`, `aShortValveNameStaysInsideTheBody`,
  `lockBorderIsBlueAndThick` and `hasLockBorder` with: a manual valve paints
  a T bar and no dome; a valve's caption is under it unless a pipe leaves
  through the bottom; a valve the example joins only top and bottom is
  vertical; a locked valve paints its badge; an unknown valve is hatched.

## Task 4: click model

Files: `canvas_items.{hpp,cpp}`, `canvas_view.{hpp,cpp}`, `test_canvas_view.cpp`.

- `ValveItem::set_on_actuate` (left click with Alt) and `set_on_select`
  (any other left click). `contextMenuEvent` and `set_on_lock_request` go.
- `CanvasView::actuate(name, op)` (explicit op) and `toggle(name)` (the old
  `on_click`); signal `valve_selected(name)`; `selected()` and the
  selection ring.
- Tests: a plain click selects and does not actuate; an Alt-click toggles
  (the old click tests, with `Qt::AltModifier`); a right click opens no menu
  and locks nothing; a read-only view selects but never actuates.

## Task 5: interlocks and `ValvePopup`

Files: `apps/nobion/src/canvas_state.{hpp,cpp}`, `core_bridge.{hpp,cpp}`,
`apps/nobion/src/valve_popup.{hpp,cpp}` (new), `apps/nobion/CMakeLists.txt`,
`canvas_view.{hpp,cpp}`, `tests/ui/test_valve_popup.cpp` (new).

- `ICanvasState::interlocks(name)` (default empty), filled in
  `CoreBridgeCanvasState` from `SwitchManager::interlock_sets()` read once.
- `ValvePopup`: name, description and kind; state line; interlock rows met
  or not; history lines; last rejection; Open/Close and Lock/Unlock buttons
  (none read-only or for a manual valve). The view owns it, shows it on
  select, refreshes it from `apply_state()` and hides it on Esc or a click
  outside.
- Tests: rows and buttons per state; Close calls actuate with Close and
  the valve closes; Lock locks; declining the unlock confirmation leaves it
  locked; read-only and manual have no buttons; interlock rows say met or
  not met, Unknown never met.

## Task 6: docs and wrap-up

Files: `docs/user/02-extraction-line.md`, spec.

- The canvas legend and the mouse table describe the bow-tie, the popup and
  Alt-click; the spec gains the caption-side rule and the popup owned by
  the view.
- `tools/quality_check.py`, `tools/compiler_sweep.py --build-dir build/dev-ui`,
  full `cmake --build --preset dev-ui` and `ctest --preset dev-ui`.

## Open questions

None.
