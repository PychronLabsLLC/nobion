# grains: Becke line overlay for sanidine separation

New app `grains` beside `elctl` and `pychron-ui`. Machine vision, augmented
reality: operator sees live microscope video of mineral grains in wintergreen
oil, racks z by hand, app labels each grain on screen with evidence score.

## 1. Purpose

Operator picks sanidine out of concentrate. Becke line check by eye slow,
per grain, tiring. App watches all grains in view at once.

Success: operator trusts labels enough to pick by them; app says unsure rather
than guess.

App advises only. Moves nothing, reads no pychron config, touches no store.

## 2. Physics, and what it does not settle

- Wintergreen oil (methyl salicylate) n about 1.537. Falls about 0.0004 per
  degree C. Values from memory: code uses neither.
- Sanidine n about 1.52: below oil. Quartz n about 1.545 to 1.55, most
  plagioclase: above oil. Albite about 1.53: near oil.
- Focus moved one way: bright Becke fringe moves toward higher-index side.
  Other way: reverses.
- Manual z gives no direction signal. Sharpness rises and falls same both ways.
  Image alone splits grains into two groups moving opposite ways, not which
  group is which. Reference grain of known side fixes sign (section 5).
- Sign says "below oil", not "sanidine". Other low-index grains read sanidine:
  volcanic glass (n about 1.49 to 1.50), microcline, orthoclase. Separation
  prep does not always remove them. So label is candidate: on screen
  `sanidine?`, in code `Label::Candidate`.
- Relief cannot settle this: microcline and orthoclase share sanidine's index.
  Fringe contrast might separate glass only, unproven. `tracks.jsonl` keeps
  contrast, so recordings can test it later.

## 3. Structure

```
libs/vision (reused)    libs/grains (new, no Qt)         apps/grains (new, Qt)
IFrameSource  ───────►  GrainPipeline::process(Frame) ─► overlay view
LiveFeed                segment, track, fringe,          recording
SyntheticSource-style   votes, classify                  grains replay
RecordedSource
```

- `libs/grains`: target `pychron_grains`, alias `pychron::grains`. Depends on
  `core` and `vision` only. Added to `PYCHRON_LIBS` after `vision`. Tests in
  `tests/grains`. No Qt; no direct OpenCV: built on `libs/vision` kernel
  (`Component`, `CircleFit`).
- `apps/grains`: binary `grains`, built under `BUILD_UI`, added beside
  `apps/pychron-ui` in top-level `CMakeLists.txt`. One window, one worker
  thread.
- `GrainPipeline` synchronous, single-threaded: one `Frame` in, one
  `GrainReport` out. No clock of its own; uses `Frame::timestamp` only, so
  output deterministic for given frames.
- Worker takes newest frame from `LiveFeed::latest()`, skips what it missed,
  posts report to UI thread. Real time by design: no simulated path, no
  `Clock::Participant`.
- Domain seam untouched: no Ar-Ar, no `Domain` entry. Not a data window.

## 4. Pipeline

Interfaces (names binding, fields may grow):

```cpp
namespace pychron::grains {

enum class Side { Below, Above };          // relative to oil
enum class Label { None, Candidate, Not, Unsure };  // None: no reference yet
enum class Vote { Same, Opposite, None };
enum class ReferenceState { None, Lost, MoveFocus, Reading, Disagree };

struct GrainView {
  std::uint64_t id;
  std::vector<vision::Vec2> outline;
  double fringe_velocity_px_s;  // positive outward
  double contrast;              // fraction of pixel_depth
  Label label;
  double score;                 // section 6; not a probability
  int passes_agree, passes_against, passes_none;
};

struct GrainReport {
  std::uint64_t seq;
  TimePoint timestamp;
  ReferenceState reference;
  std::vector<GrainView> grains;
  std::string warning;          // empty: none
};

class GrainPipeline {
 public:
  explicit GrainPipeline(GrainOptions options);
  GrainReport process(const vision::Frame& frame);
  Result<void> set_reference(vision::Vec2 frame_px, Side side);
  Result<void> set_second_reference(vision::Vec2 frame_px, Side side);
  void clear_reference();
};

}  // namespace pychron::grains
```

Per frame, in order:

1. Segment. Grains as blobs. Low-relief grains in near-matching oil have faint
   edges: segment on gradient accumulated over recent frames, not one frame.
2. Track. Match blobs frame to frame by overlap; stable id per grain. Id
   survives gap up to `track_gap_frames`.
3. Fringe. Resample ring around outline into radial brightness profile. Signed
   fringe velocity `v(t)`, positive outward, plus contrast. Measured relative
   to tracked outline, so slow drift harmless.
4. Vote. Sign changes of reference `v_ref(t)` split time into passes. Per pass,
   per grain: `Same` or `Opposite` from sign of `v * v_ref`; `None` when
   contrast below `min_contrast`. Vote weighted by contrast relative to
   reference's.
5. Classify. Section 6.

Global frame shift above `max_shift_px` within pass: pass dropped, no vote from
it.

Rule: comparison with reference at same moment is only use of z. No encoder,
no focus model, no assumed direction of rack.

## 5. Reference

- Operator drops known material in mount, clicks grain, declares side.
  Standard sanidine `Below`, quartz `Above`. Side remembered between sessions.
- Reference refused, with reason, when its contrast below
  `min_reference_contrast`. Near-oil pick fails early.
- `ReferenceState`, in banner always:
  - `None`: grains outlined grey, no labels, no votes.
  - `Lost`: votes stop, labels greyed and held, until tracked again or
    re-clicked.
  - `MoveFocus`: reference ok, no pass running.
  - `Reading`: passes counted.
  - `Disagree`: section below.
- No `Candidate` or `Not` label exists while no reference has been set. Label
  without sign worthless.
- Wrong side declared inverts every label. App cannot detect alone. Banner
  always shows reference and declared side.
- Optional second reference of opposite side: their fringes must move opposite.
  If they do not over `disagree_passes` passes: state `Disagree`, labels stop.

## 6. Label and score

```
score = (agree + 1) / (agree + against + 2)
```

`agree` and `against`: contrast-weighted votes for grain's leading side. `None`
votes add to neither.

Side from reference: `Same` puts grain on reference's side, `Opposite` on
other. `Below` is `Candidate`, `Above` is `Not`.

- Label is `Candidate` or `Not` only when grain has at least `min_passes` voted
  passes and `score >= min_score`. Otherwise `Unsure`. Defaults: 3, 0.8.
- Grain with weak fringe in every pass stays `Unsure`: correct outcome for
  near-oil grains.
- Track lost past `track_gap_frames`: votes reset. Rediscovered blob may be
  another grain.
- Score is evidence score, not probability. Becomes calibrated only after
  recorded grains of known identity measure it. UI says "score".

## 7. Screen

- Video fills window. Each grain: outline, label text, score, tick per pass.
- Label text: `sanidine?`, `not`, `unsure`. Never bare `sanidine`.
- Colours: candidate green, not red, unsure amber, grey without reference. Text
  always present: colour never sole signal. Colours from named tokens, no
  literals scattered in paint code.
- Click grain: pass history, contrast, why unsure.
- Click-to-pin: operator marks grain picked, grain greyed. Session only.
- Click on grain with reference tool: set reference, side chooser.
- Banner: reference state, reference id, declared side, warning.

## 8. Recording and replay

- Toggle on screen. Writes folder: one PGM per frame through `vision` `pgm`,
  manifest `RecordedSource` reads (`fixture.hpp`), plus `tracks.jsonl`: per
  frame, per grain outline, `v`, contrast, vote, label, score, reference state.
- `tracks.jsonl` keeps enough to train model later. No model in this spec.
- After writing folder: `pychron::mark_as_user_file` on each user-opened file
  (`tracks.jsonl`, manifest).
- `grains replay <folder>` headless, prints per-grain summary. Same pipeline.
  Reference for replay from `tracks.jsonl`.
- Rule: replay of recording gives same report as live gave on same frames.

## 9. Failure

- Camera lost: `LiveFeed::Latest::error`. Last picture greyed under reason (as
  `apps/pychron-ui` `CameraView`), votes stop, labels held greyed. Returns by
  itself on reopen.
- Frame rate under `min_fps` (default 10): warning `camera too slow`, passes
  not counted.
- Global shift: pass dropped (section 4).
- Processing behind camera: frames dropped, never queued.
- Bad `grains.toml`: `Result` error shown at start, app does not run on
  defaults silently.

## 10. Config

`grains.toml`, own file, beside binary's working directory or given by
`--config`:

- `[camera]`: maps to `vision::CameraRequest` (backend, device, width, height,
  fps, options).
- `[pipeline]`: `min_contrast`, `min_reference_contrast`, `min_passes`,
  `min_score`, `track_gap_frames`, `max_shift_px`, `disagree_passes`,
  `min_fps`.

Defaults for all but `[camera]`.

## 11. Camera requirements

No hardware yet; these choose it.

- Mono, or colour read as luma. 2 MP or more. Global shutter preferred. 15 fps
  or more. USB3 or GigE on existing backends (`opencv`, `pylon`).
- Transmitted light, condenser stopped down: Becke contrast depends on it.
- Fixed exposure and gain. Auto-exposure ramps read as fringe change.
- Grain at least about 80 px across at chosen magnification. Reasoned, not
  measured: confirm on bench.

## 12. Testing

`GrainSynth` scene in `libs/grains` (carries Becke physics, so not in
`libs/vision`): rendered grains of known side, fringes driven by scripted z,
noise, contrast. Implements `vision::IFrameSource`.

Rules with a test each:

- No reference, no label.
- Reversing z script leaves every label unchanged.
- Flipping declared side inverts `Candidate` and `Not`.
- Near-oil grains stay `Unsure`.
- Irregular z speed, irregular reversals: same labels.
- Reference lost: no new votes, labels held.
- Second reference moving same way: `Disagree`, labels stop.
- Global shift drops pass.
- Replay equals live on same frames.
- Track lost past gap resets votes.

Validation gate, not a test: segmentation and fringe measure on real faint
edges unverified until hardware exists. First recorded sweeps of known grains
decide thresholds and whether step 1 of section 4 holds. Until then app not
called working.

## 13. Out of scope

Stage or z control. Encoder input. Air-bubble detection. Trained model. Named
minerals. Telling sanidine from glass, microcline or orthoclase (relief cue,
crossed polars). Sample records, operator or oil metadata. `libs/persistence`, DVC
store. Phone, tablet. Installer or packaging: `grains` runs from a build until
hardware proves it. Colour pipeline: `Frame` stays mono.

## Open questions

None.
