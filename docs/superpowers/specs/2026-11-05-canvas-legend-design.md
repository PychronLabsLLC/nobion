# Canvas glyph legend

Date 2026-11-05. Status: built. Follows `2026-11-04-hmi-valves-design.md`.

## 1. Why

Canvas symbols (bow-tie, dome, T bar, hatch, dashed ring, padlock, gauge
chip, volume glyphs, section colours) explained only in user guide
(`docs/user/02-extraction-line.md`, "Reading the canvas"). Operator at the
line has no key. Goal: button on canvas opens legend of every glyph, drawn
by canvas's own items so it cannot drift from what canvas paints.

## 2. Scope

In:

- "Legend" button on every `CanvasView` (main window, rehearsal window).
- `CanvasLegend` window: one row per glyph, picture plus meaning, in
  sections.
- Section colours of this canvas: one row per source.

Out:

- `[legend]` table of `canvas.toml` (parsed, `Canvas::legend`, never
  drawn): stays parsed and undrawn. Legend drawn inside the drawing is
  separate spec if wanted.
- Shortcut for legend.
- `[[label]]` and `[[image]]`: decoration, no meaning to explain.

## 3. Button

- `QToolButton` "Legend", checkable, child of `viewport()`, top-right
  corner, 8 px margin, raised over scene; moved in `resizeEvent`. Object
  name `canvasLegendButton`. Tooltip "Show what the canvas symbols mean".
  Framed at rest (`style::make_outlined`, property `outlined`): app sheet
  draws a bare `QToolButton` as text (changed 2026-11-05, developer's
  review). No icon.
- Help > Canvas Legend (`MainWindow::legend_action()`, app-wide: in every
  window's Help menu) does `show_legend(true)` on main window's canvas.
  Rehearsal window's canvas: button only.
- Click toggles legend. Button checked exactly while legend visible
  (close by title bar or Esc unchecks it; window shown again with its
  parent checks it).
- Read-only view has it too: legend shows nothing a button could act on.
- `CanvasView::legend_button()`, `CanvasView::legend()` (null until first
  shown), `CanvasView::show_legend(bool)`.

## 4. `CanvasLegend`

New `apps/nobion/src/canvas_legend.{hpp,cpp}`.

- `QWidget`, top-level `Qt::Tool` window, parent canvas view, title
  "Canvas legend". Non-modal: stays open while operator works canvas.
  Made on first show, owned by view (Qt parent); hidden, not deleted, on
  close.
- Esc closes it. `shown_changed(bool)` from `showEvent` / `hideEvent`;
  button's checked state set from it, so it also follows the window
  hiding and coming back with its parent window.
- Content: `QScrollArea` holding grid; per section a bold header, then rows
  of glyph (`QLabel` with pixmap) and meaning (`QLabel`, word wrap).
- `set_entries(std::vector<LegendEntry>)` rebuilds; `entries()` for tests.

```cpp
struct LegendEntry {
  QString section;  // "Valves", "Gauges", "Volumes", "Pipes", "Section colours"
  QString key;      // stable id for tests: "valve.open", "stage.turbo", "source.<name>", "section.none"
  QString text;     // what it means
  QPixmap glyph;
};
```

Entries built by free function, so view and tests share it:

```cpp
struct LegendInputs {
  canvas::ValveStyle valve_style = canvas::ValveStyle::Hmi;
  canvas::OpenValveColor open_valve_color = canvas::OpenValveColor::Green;
  double connection_width = 5;
  std::optional<QColor> background;  // canvas's; unset: theme().base
  struct Source { QString key; QString label; QColor color; };  // key: best source's name
  std::vector<Source> sources;       // in precedence order, highest first
  qreal device_pixel_ratio = 1.0;
};
std::vector<LegendEntry> legend_entries(const LegendInputs& in);
```

## 5. Glyphs

Each glyph: real canvas item(s) placed in a scratch `QGraphicsScene`,
rendered (`QGraphicsScene::render`) into pixmap, `kGlyphWidth` 72 x
`kGlyphHeight` 44 logical px, at `device_pixel_ratio`, background
`LegendInputs::background`. Items scaled 1:1 (canvas units = px) and
centred; never a hand-drawn copy of a glyph.

Valve glyphs: `ValveItem` with empty label (no caption), style from inputs,
pipe sides Left|Right, horizontal `ConnectionItem` stub through it (and its
`outline()`), width `connection_width`, `ConnectionItem::default_color()`.

Rows, in order:

Valves:

| key | item | text |
|---|---|---|
| `valve.actuated` | Valve, Closed | Valve Nobion drives (dome on stem). Lies along its pipe; upright when pipes leave only above and below. |
| `valve.manual` | Manual, Closed | Manual valve (T bar): moved by hand, its state told to Nobion. |
| `valve.switch` | Switch, Closed | Switch: pump power, heater relay, shutter. |
| `valve.open` | Valve, Open | Open. |
| `valve.closed` | Valve, Closed | Closed. |
| `valve.unknown` | Valve, Unknown | State unknown: not read yet, or last command or read-back failed. |
| `valve.pending` | Valve, Closed, pending | Command in flight. |
| `valve.selected` | Valve, Closed, selected | Faceplate open (rehearsal: last step changed it). |
| `valve.locked` | Valve, Closed, locked | Locked in software: refuses open and close until unlocked from faceplate. |
| `valve.failed` | Valve, Closed, `flash("")` | Flashing: last command refused or failed; reason in tooltip and faceplate. |

Open and closed texts name box and glyph (boxed valves,
`2026-11-04-hmi-valves-design.md` §4): hmi open "white glyph, box in the
colour of the section it joins (light grey when that section has no
source)", closed "grey box, black glyph"; redgreen "green box" / "red
box", open in section colour with `open_valve_color = "inherit"`. Open
glyph's box takes first of `LegendInputs::sources` (pump's colour when
none) whenever open box would take a section's colour.

Gauges (`GaugeLabelItem`, name "IG"):

| key | state | text |
|---|---|---|
| `gauge.reading` | wired, `set_value(1.2e-8, "torr")` | Gauge and its latest reading. Rest pointer for its last 10 minutes; click for its window. |
| `gauge.alarm` | wired, value, alarm | Reading outside its alarm limits. |
| `gauge.unwired` | not wired | Drawn but not defined by the line: no reading. |

Volumes (`StageItem`, 56 x 36, empty label, `isolated_color()`):

| key | symbol | text |
|---|---|---|
| `stage.volume` | None | Volume: stage, pipette, tank. Filled with its section colour. |
| `stage.<symbol>` | each `StageSymbol` but None, toml spelling (`spectrometer`, `quadrupole`, `laser`, `turbo`, `getter`, `ion_pump`, `cryo`, `roughing_pump`) | Its name: "Spectrometer", "Quadrupole", "Laser", "Turbo pump", "Getter", "Ion pump", "Cryo", "Roughing pump". |

Glyph needs label-free room in a 56 x 36 box; if `symbol_rect` gives
empty for an empty label, box grows until it does not (test holds every
symbol row draws one).

Pipes:

| key | item | text |
|---|---|---|
| `pipe` | horizontal `ConnectionItem`, `default_color()` | Pipe. Takes the colour of the section it is in. |

Section colours (`StageItem` box, as volumes, `set_region_color`):

- `source.<name>` per colour among sources of this canvas, in
  `LegendInputs::sources` order, `<name>` its best-ranked source; text
  "<label>: gas from it, or pumped by it". Sources sharing a colour (every
  pump) share a row, in place of best of them. Label: display names (or
  names) joined by ", ", kind in brackets once when all one kind
  ("Ion pump, rough, turbo, turbo2 (pump)"), else after each. Colour: what
  `apply_state` paints source's own region: own `color`, else its tank's,
  else `source_color(kind, ordinal)`. One function computes it for both
  (`CanvasView::source_display_color`), never two copies.
- `section.none`, `isolated_color()`: "No source: isolated.".
- Footnote row text (no glyph, key `section.rule`): "Where sections meet
  through open valves, the source higher in this list wins." Fixed rows
  `section.*`, not `source.*`: a source may be named `none`.

Order of sources: rule of `canvas::dominant` (precedence, then kind's
place, then name), highest first.

## 6. Live

View rebuilds entries when shown and on `set_valve_style` /
`set_open_valve_color` while legend exists. Nothing else changes them:
legend shows symbols, not state.

## 7. Rules that must hold

- Every glyph from canvas item classes; no paint code in `canvas_legend`.
- Legend reaches no bridge, no `ICanvasState`: inputs are values.
- Button never covers a click target permanently: it is on viewport, above
  scene, top-right; canvas is fitted with `kContentMargin` 20 and the
  button sits inside that margin's corner only when drawing reaches it.
  Accepted.
- Domain seam untouched.

## 8. Tests

`tests/ui/test_canvas_legend.cpp` (new):

- Button exists, top-right of viewport; click shows legend, checked; close
  unchecks; read-only view has it.
- Every key in §5 present, sources match the example canvas's sources.
- Source row colour equals that source stage's `region_color()` on canvas;
  no two rows one colour; pumps one row.
- Glyph pixels: open valve fill per style (hmi `muted_text`, redgreen `ok`)
  at centre; locked row has `lock_color()` pixel in badge; style switch
  rebuilds.
- Every symbol row glyph differs from `stage.volume` glyph.

User guide: `docs/user/02-extraction-line.md` "Reading the canvas" names
button; `01-getting-started.md` canvas bullets.

## Open questions

None.
