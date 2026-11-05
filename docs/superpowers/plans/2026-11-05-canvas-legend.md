# Canvas glyph legend: plan

Spec: `docs/superpowers/specs/2026-11-05-canvas-legend-design.md`.

## Task 1: one ranking rule for sources

`canvas::outranks(const Source&, const Source&)` in
`libs/systems/include/pychron/systems/canvas/canvas.hpp` and
`libs/systems/src/canvas/sources.cpp`: higher precedence, then earlier kind,
then the smaller name. `dominant()` uses it. Test in
`tests/systems/test_canvas_sources.cpp` (`OutranksIsTheRuleDominantUses`).

## Task 2: one colour for a source

`CanvasView::source_display_color(const Source&, sources)`: the source's own
colour, else its tank's, else the theme's. `apply_regions()` calls it. No
behaviour change: the existing canvas view tests cover it.

## Task 3: legend entries

`apps/nobion/src/canvas_legend.{hpp,cpp}`: `LegendEntry`, `LegendInputs`,
`legend_entries()`, rendered from canvas items in a scratch scene. Added to
`apps/nobion/CMakeLists.txt`. Tests in `tests/ui/test_canvas_legend.cpp`:
every key, glyph pixels per style, symbol glyphs differ from the plain box.

## Task 4: legend window and button

`CanvasLegend` window in the same files; `CanvasView` gets the button, the
legend, `show_legend()`, `legend_inputs()`, rebuild on style changes. Tests:
button place, toggle and check state, Esc, read-only view, source rows match
the canvas's region colours.

## Task 5: user guide

`docs/user/02-extraction-line.md` and `docs/user/01-getting-started.md`.

## Open questions

None.
