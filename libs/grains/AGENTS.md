# Grains (`libs/grains`, `apps/grains`): agent rules

Part of the agent guide: root `AGENTS.md` applies in full, this adds what is
only this component's. Kept compressed, as that file is. Spec:
`docs/superpowers/specs/2026-10-30-grains-design.md`.

- `GrainPipeline` has no clock and no z. Time is `Frame::timestamp` only; z
  is never read, modelled or assumed: only comparison with reference grain at
  same moment gives sign. Code that reads a clock, an encoder or a focus
  direction is wrong here, however convenient.
- No `Candidate` or `Not` label without a reference. `Label::None` until one
  is set. A label with no sign is worthless.
- On screen `sanidine?`, never bare `sanidine`: `Label::Candidate` means
  "below oil", and glass, microcline and orthoclase read the same. Text
  always present, colour never the only signal (`overlay_tokens.hpp`).
- `score` is evidence, not probability: say "score" in UI and docs, never
  "confidence" or a percent, until recorded grains of known identity
  calibrate it.
- Replay equals live (`replay_folder` checks it). So any new input to
  `process`, or operator action on the pipeline, goes in `tracks.jsonl`
  (`RecordEvent`) and is applied by replay before the frame it preceded. New
  field of a report that replay compares goes in `report_json`. Starting a
  recording calls `reset()` and logs a `Restore` event, so replay starts
  where live did.
- `Segmenter` assumes a line only adds light. A real line's dark band is
  untested. Constants (`GrainOptions` defaults, `segment_window`, quantile,
  fringe gain in `GrainSynth`) tuned on synthetic scene only: first recorded
  sweeps of known grains decide them (spec section 12). Until then app not
  called working; do not retune against `GrainSynth` and call it done.
- `libs/grains` has no Qt and no OpenCV, depends on `core` and `vision` only.
  Real time on purpose in `GrainRunner` (no `Clock::Participant`): do not
  put it on a simulated path.
- App advises only: no stage or z control, no pychron config, no store.
