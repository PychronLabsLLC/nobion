# HMI valves on the extraction line canvas

Date 2026-11-04. Status: built. Mockup agreed in session 2026-11-04.

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
# valve fill: "hmi" (default; glyph white open, black closed) or "redgreen"
valve_style = "redgreen"
```

- `canvas::ValveStyle { Hmi, Redgreen }`, field `Canvas::valve_style`,
  default `Hmi`. Read in `libs/systems/src/canvas/loader.cpp` with
  `read_enum` beside `open_valve_color`; added to the `reject_unknown` key
  list. Unknown value: load error, as `open_valve_color`.
- Default `Hmi`: every line without the key changes look on upgrade. Wanted.
- `configs/examples/canvas.toml` gets commented key. NMGRL example left as
  is: written by `tools/rebuild_nmgrl_example.py`, takes default.
- `CanvasView::valve_style()` / `set_valve_style()`, mirror of
  `open_valve_color` pair: initialised from file, setter repaints.

## 4. Glyph (`ValveItem::paint`)

Body box: `kSize` 30 x 30, centred on element position, drawn (changed
2026-11-05, developer's review of the first build): rounded rect, radius 3,
1 px `theme().outline`, filled with state's box colour (below). What a click
hits (`shape()`). Pipes still join any side. Glyph inside it, scaled 0.72
(`kGlyphScale`), its sizes below before scaling:

- Bow-tie: two triangles tip to tip at centre, each 13 wide, 18 tall, axis
  along the pipe. 1.5 px outline in `theme().text`.
- Actuator mark on stem from centre, perpendicular to axis:
  - pneumatic (every kind but Manual): stem to 10 from centre, half-disc
    dome radius 5 on it (top at box edge), fill `theme().base`.
  - Manual: stem to 12, T bar 14 wide, 2 px (handwheel); replaces face
    handwheel and `kWheelRadius`, `kCornerWheelRadius`, `wheel_rect()`.
- Pipe sides: `CanvasView::build`, as each pipe is laid
  (`note_pipe_side`), records side each pipe leaves valve by: first point of
  its run more than 1 unit from valve centre, dominant axis. Pipe end more
  than `kSize` from valve (tee stem meeting bar) counts no side. Pushed with
  `ValveItem::set_pipe_sides(mask)` after all pipes.
- Orientation: vertical when sides non-empty and none left or right; stem
  then to left. A switch joining nothing (`pump_power`) has no sides.
- Box colour by state (`fill_color()`; `valve_fill(state, style)` when no
  section colour):

| State | `hmi` | `redgreen` |
|---|---|---|
| Open | section colour of region it joins; `theme().neutral_fill` when region has no source | `theme().ok`; section colour with `open_valve_color = "inherit"` |
| Closed | `theme().inactive` (grey) | `theme().error` |
| Unknown | hatch: `theme().muted_text` 45° lines on `theme().base` | same |

- Glyph fill, both styles (`valve_tie_fill(state)`): Open
  `theme().valve_open` (white), Closed `theme().valve_closed` (black),
  Unknown `theme().muted_text`. State reads without box colour:
  open valve in a sourceless region has a grey box as a closed one does,
  but a white glyph.

Pending (command in flight): dashed ring around body, fill unchanged (item
does not know target state).

- hmi: open box always takes region colour (`set_inherited_color`, pushed
  by `apply_regions`); `open_valve_color` matters only for `redgreen`.
- Flash on rejection: box alternates with `theme().flash`.
- Marked (rehearsal) unchanged: accent outline outside body.
- Label: never on face (bow-tie has no room). Always caption, scene font
  unshrunk, on first side of below, above, right, left that no pipe leaves
  by; below when all four have one. Empty label (manual valve without
  `display_name`): no caption. `caption_rect()`, `boundingRect()` takes it
  in; click area stays body (`shape()`).
- Badges, top-right corner of body box, 12 x 12, over glyph:
  - locked: padlock (body filled, shackle stroked, `theme().lock`) on a
    `theme().base` square. Replaces 5 px lock border; `kLockBorderWidth`
    removed.
  - interlocked: not drawn on glyph (popup shows it).

`ValveKind::Switch` (not a valve: pump, heater power): same box, a circle
inside it, no bow-tie, no stem; box, glyph fill, pending, badge and label
rules as above. Rings (pending, selected, marked) rounded rects for all
kinds.

`valve_color(ValveState)` becomes `valve_fill(ValveState, ValveStyle)`;
only callers besides `ValveItem::fill_color` are in
`tests/ui/test_canvas_view.cpp`.

## 5. Input

`ValveItem::mousePressEvent`, left button:

- Alt held (`Qt::AltModifier`; Option on macOS): `on_actuate_(name)`. Same
  path as today's click: `CanvasView::on_click` renamed `toggle`,
  rules unchanged (read-only, pending, unknown name: nothing).
- No modifier: `on_select_(name)`. View emits
  `valve_selected(name)`; the view shows its `ValvePopup` (§6).
- Other modifiers: as no modifier.

Right button: ignored (event passes on). `contextMenuEvent` and
`set_on_lock_request` removed. `CanvasView::request_lock` stays: popup calls
it; unlock still asks `confirm_unlock_` first.

Read-only view (null bridge, rehearsal): left click opens popup without
operate or lock buttons; Alt-click does nothing.

Tooltip: unchanged (name, `valve_details`, last rejection).

Selected valve shows accent ring (as marked) while its popup is open.

Popup owned by `CanvasView` (made on first `select`), not main window: the
rehearsal window's read-only canvas gets it too. `CanvasView::select(name)`
selects and shows it; `valve_selected` signal says so on a click;
`ValvePopup::closed` ends selection. `CanvasView::faceplate(name)` builds
what it shows (`ValveFaceplate`); popup holds no line state.

## 6. `ValvePopup`

New `apps/nobion/src/valve_popup.{hpp,cpp}`. `QFrame`, `Qt::Popup` window
(takes focus; Esc or click outside closes it; unlike `GaugePopup`, which
must not take clicks). Parented to canvas view. One at a time: selecting
another valve moves it. Placed beside valve's global rect, kept on screen
(`GaugePopup::placed`).

Content, top to bottom:

1. Name (label if set), description, kind ("pneumatic", "manual").
2. State line: `valve_details` state line ("Open since 13:41:07 (22 min)",
   owner).
3. Interlocks (valves with partners only): one row per partner, "Opens
   only with <name> closed: met" (exclusive) or "... <name> open: ..."
   (positive), met / not met against current state, rule of
   `check_switch_interlocks` (Unknown never meets). Closing never
   interlocked.
4. History: rest of `valve_details` (last actuated, counts, time open).
5. Last rejection reason, if any (what flash shows), in `error_text`.
6. Buttons (not in read-only, not for Manual):
   - Operate: "Close" when Open, "Open" when Closed, both when Unknown.
     Calls `CanvasView::actuate(name, op)` (explicit op, no toggle).
     Disabled while pending, line "Moving…". Disabled while locked, with
     line "Locked: unlock to operate". Popup stays open after operate.
   - Lock / Unlock: `request_lock(name, !locked)`. Unlock question is a
     dialog, which closes a popup: view selects valve again after it.

Popup sets `Qt::WA_NoMouseReplay`: click outside that closes it is not
replayed (on the valve it would reopen it). View destructor disconnects and
deletes popup first (its hide ends selection, which reads valves).

Live: view pushes `ValvePopup::refresh()` from `apply_state()` while shown;
popup reads state through `ICanvasState`, never a clock of its own
(`wall_now()`).

Interlock partners: `ICanvasState::interlocks(name)` returns
`systems::InterlockSet` (empty when none); `CoreBridge` reads
`SwitchManager::interlock_sets()` once, in its constructor.

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
`tests/systems/test_canvas_loader.cpp` beside `open_valve_color` cases:

- `valve_style` parsed; default `Hmi`; bad value refused.
- Box colour per state per style; hmi open takes section colour always,
  redgreen only with inherit; glyph white open, black closed.
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
