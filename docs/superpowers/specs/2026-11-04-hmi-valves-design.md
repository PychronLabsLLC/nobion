# HMI valves on the extraction line canvas

Date 2026-11-04. Status: draft. Mockup agreed in session 2026-11-04.

## 1. Why

Canvas valve = rounded box, green open, red closed. Closed reads as alarm.
Click actuates at once: one stray click moves gas. Lock behind right-click
menu, details only in tooltip. Goal: ISA-101-style valve (ISA-5.1 bow-tie,
state by shape and fill), click selects and opens faceplate, operating is
deliberate.

## 2. Scope

In:

- Valve glyph: bow-tie, two fill styles (`hmi` default, `redgreen` per line).
- Click model: left click opens valve faceplate popup; Option-click (Alt)
  actuates as left click does today.
- Valve faceplate popup (`ValvePopup`): state, history, interlocks, lock,
  operate.
- Lock/unlock moves from context menu to popup. Context menu removed.

Out:

- Section (region) colouring: unchanged. Pipes, volumes, `open_valve_color`
  behave as today.
- Window status banner, alarm summary strip, gauge faceplates, stale-reading
  mark, flow animation: separate spec if wanted. Alarms have severity only,
  no priority: banner needs that settled first.
- Per-change valve history (list of who changed what when): not stored.
  Popup shows what is stored (`SwitchStats`), as tooltip does.
- Who locked a valve and when: not stored; popup shows locked yes/no.
- Two-step select/confirm inside popup: popup itself is the select step.

## 3. canvas.toml

New key in `[canvas]`:

```toml
# valve fill: "hmi" (default; open filled grey, closed hollow) or "redgreen"
valve_style = "redgreen"
```

- `canvas::ValveStyle { Hmi, Redgreen }`, field `Canvas::valve_style`,
  default `Hmi`. Read in `libs/systems/src/canvas/loader.cpp` with
  `read_enum` beside `open_valve_color`; added to the `reject_unknown` key
  list. Unknown value: load error, as `open_valve_color`.
- Default `Hmi`: every line without the key changes look on upgrade. Wanted.
- `configs/examples/canvas.toml` and `configs/examples/nmgrl/canvas.toml`
  get the commented key.
- `CanvasView::valve_style()` / `set_valve_style()`, mirror of
  `open_valve_color` pair: initialised from file, setter repaints.

## 4. Glyph (`ValveItem::paint`)

Body box unchanged: `kSize` 30 x 30, centred on element position; what a
click hits (`shape()`). Pipes still join any side. Inside it:

- Bow-tie: two triangles tip to tip at centre, each 13 wide, 18 tall, axis
  along the pipe. 1.5 px outline in `theme().text`.
- Actuator mark on stem from centre, perpendicular to axis, to box edge:
  - pneumatic (every kind but Manual): half-disc dome, radius 7, fill
    `theme().base`.
  - Manual: T bar 16 wide (handwheel), replaces face handwheel and
    `kWheelRadius`, `kCornerWheelRadius`, `wheel_rect()`.
- Orientation: horizontal unless every pipe meeting the valve leaves it
  through top or bottom of body box (first segment of its polyline at the
  valve); then vertical, stem to left. Worked out in `CanvasView::build`
  after pipes, pushed with `ValveItem::set_vertical(bool)`.
- Fill by state:

| State | `hmi` | `redgreen` |
|---|---|---|
| Open | `theme().muted_text` | `theme().ok` |
| Closed | `theme().base` (hollow) | `theme().error` |
| Unknown | hatch: `theme().faint_text` 45° lines on `theme().base` | same |
| Pending (moving) | half: target side filled, outline dashed | same, colours of style |

- `open_valve_color = "inherit"`: open fill is region colour in both
  styles (rule as today, `set_inherited_color`).
- Flash on rejection unchanged: fill alternates with `theme().flash`.
- Marked (rehearsal) unchanged: accent outline outside body.
- Label: never on face (bow-tie has no room). Always caption under body, as
  today's long-name caption (`caption_rect()`, `boundingRect()` takes it
  in). `NameLayout` keeps shrink rule for caption width only.
- Badges, top-right corner of body box, 12 x 12, over glyph:
  - locked: padlock, outline `theme().lock`. Replaces 5 px lock border;
    `kLockBorderWidth` removed.
  - interlocked: not drawn on glyph (popup shows it).

`ValveKind::Switch` (not a valve: pump, heater power) keeps circle body, no
bow-tie, no stem; fill, pending, badge and label rules as above.

`valve_color(ValveState)` becomes `valve_fill(ValveState, ValveStyle)`;
only callers besides `ValveItem::fill_color` are in
`tests/ui/test_canvas_view.cpp`.

## 5. Input

`ValveItem::mousePressEvent`, left button:

- Alt held (`Qt::AltModifier`; Option on macOS): `on_actuate_(name)`. Same
  path as today's click: `CanvasView::on_click` renamed `actuate_toggle`,
  rules unchanged (read-only, pending, unknown name: nothing).
- No modifier: `on_select_(name, global body rect)`. View emits
  `valve_selected(name, global_rect)`; main window shows `ValvePopup`.
- Other modifiers: as no modifier.

Right button: ignored (event passes on). `contextMenuEvent` and
`set_on_lock_request` removed. `CanvasView::request_lock` stays: popup calls
it; unlock still asks `confirm_unlock_` first.

Read-only view (null bridge, rehearsal): left click opens popup without
operate or lock buttons; Alt-click does nothing.

Tooltip: unchanged (name, `valve_details`, last rejection).

Selected valve shows accent outline (as marked) while its popup is open.

## 6. `ValvePopup`

New `apps/nobion/src/valve_popup.{hpp,cpp}`. `QFrame`, `Qt::Popup` window
(takes focus; Esc or click outside closes it; unlike `GaugePopup`, which
must not take clicks). Parented to main window. One at a time: selecting
another valve moves it. Placed beside valve's global rect, kept on screen.

Content, top to bottom:

1. Name (label if set), description, kind ("pneumatic", "manual").
2. State line: `valve_details` state line ("Open since 13:41:07 (22 min)",
   owner).
3. Interlocks (valves with partners only): one row per partner, "<name>
   must be closed" (exclusive) or "<name> must be open" (positive), mark
   met / not met against current state, rule of
   `check_switch_interlocks` (Unknown never meets). Closing never
   interlocked: rows say "to open".
4. History: rest of `valve_details` (last actuated, counts, time open).
5. Last rejection reason, if any (what flash shows), in `error_text`.
6. Buttons (not in read-only, not for Manual):
   - Operate: "Close" when Open, "Open" when Closed, both when Unknown.
     Calls `CanvasView::actuate(name, op)` (explicit op, no toggle).
     Disabled while pending, label "Moving…". Disabled while locked, with
     line "Locked: unlock to operate". Popup stays open after operate.
   - Lock / Unlock: `request_lock(name, !locked)`.

Live: view pushes `ValvePopup::refresh()` from `apply_state()` while shown;
popup reads state through `ICanvasState`, never a clock of its own
(`wall_now()`).

Interlock partners: `ICanvasState::interlocks(name)` returns
`systems::InterlockSet` (empty when none); `CoreBridge` fills it from
`SwitchManager::interlock_sets()`, fixed at load, read once.

## 7. Rules that must hold

- No path from plain left click to actuation. Only Alt-click and popup
  Operate actuate.
- Read-only view: popup has no button that reaches bridge.
- Unlock always asks confirmation, from popup as before from menu.
- Locked valve: popup Operate disabled; Alt-click still sent, core refuses
  and valve flashes (as today: lock enforced in core, not UI).
- Body box size and centre unchanged: no `canvas.toml` layout moves.
- Domain seam untouched: nothing Ar-Ar.

## 8. Tests

`tests/ui/test_canvas_view.cpp`, `tests/ui/test_valve_popup.cpp` (new),
loader test beside existing `open_valve_color` cases:

- `valve_style` parsed; default `Hmi`; bad value refused.
- Fill per state per style; inherit wins for open in both.
- Plain click: no actuation, `valve_selected` emitted. Alt-click: actuation
  as old click test.
- Right click: no menu, no lock.
- Popup: buttons per state; operate calls actuate with op; lock calls
  `request_lock`; unlock declined by confirm: nothing sent; read-only: no
  buttons; Manual: no buttons.
- Interlock rows met / not met from state, Unknown not met.
- Orientation: valve joined only top/bottom is vertical.

## Open questions

None.
