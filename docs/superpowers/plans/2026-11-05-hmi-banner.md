# HMI status banner, alarm strip, stale gauge mark: plan

Spec: `docs/superpowers/specs/2026-11-05-hmi-banner-design.md`.

## Task 1: one staleness rule

`systems::gauge_stale_after(const config::SystemSection&, int scans)` and
`kGaugeStaleScans` in `libs/systems/include/pychron/systems/line_pressure_service.hpp`;
`LinePressureService::get_pressure` uses it. Test in the pressure service's
suite under `tests/systems`.

## Task 2: what the banner counts

`AlarmDock::rows()` and `changed()`; `HealthBar::summary()` and
`changed()`. Tests beside the existing dock and health bar tests in
`tests/ui`.

## Task 3: banner and strip

`apps/nobion/src/status_banner.{hpp,cpp}`, `apps/nobion/src/alarm_strip.{hpp,cpp}`,
`LineMode` and `MainWindow::set_line_mode` (called from `main.cpp`), the
central wrapper in `main_window.cpp`, executor text from `ExperimentBridge`
signals. Tests `tests/ui/test_status_banner.cpp`, `tests/ui/test_alarm_strip.cpp`.

## Task 4: stale gauge mark

`ICanvasState::gauge_stale_after()`, `CoreBridge` reads it once;
`GaugeLabelItem::set_stale`; `CanvasView` 1 s check; legend row
`gauge.stale`. Tests in `tests/ui/test_canvas_view.cpp` and
`tests/ui/test_canvas_legend.cpp`.

## Task 5: user guide

`docs/user/02-extraction-line.md`, `docs/user/01-getting-started.md`.

## Open questions

None.
