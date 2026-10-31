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
- Worker (`GrainRunner`, `libs/grains`, Qt-free) polls a function returning
  `FrameTick` (frame or error string). App's poll (`live_poll`) calls
  `LiveFeed::latest_if_newer(seq)`: frame copied out only when its `seq` not
  the last handed on, so idle poll copies nothing. Runner also skips a frame
  whose `seq` it already processed. Frames missed while busy are dropped.
  Report left in mailbox (`Shown`: frame, report, camera error,
  `recording_error`) under own mutex; newest only, older dropped. Taker woken
  by callback that carries nothing and runs on worker; taker marshals to own
  thread. Operator commands (set or clear reference, start or stop recording)
  queue and run on worker between frames: only worker touches pipeline and
  recording. Start and stop wait for worker and return its `Result`; called
  from a worker callback they fail with `ErrorKind::Config`. Real time by
  design: no simulated path, no `Clock::Participant`.
- `grains --demo`: window on synthetic scene, no camera, no config. App can be
  seen before hardware exists.
- Domain seam untouched: no Ar-Ar, no `Domain` entry. Not a data window.

## 4. Pipeline

Interfaces (names binding, fields may grow):

```cpp
namespace pychron::grains {

enum class Side { Below, Above };          // relative to oil
enum class Label { None, Candidate, Not, Unsure };  // None: no reference yet
enum class Vote { Same, Opposite, None };
enum class ReferenceState { None, Lost, MoveFocus, Reading, Disagree };

struct ReferenceMark { vision::Vec2 at; Side side; };

struct GrainView {
  std::uint64_t id;
  std::vector<vision::Vec2> outline;
  double fringe_velocity_px_s;  // positive outward
  double contrast;              // fraction of pixel_depth
  Label label;
  double score;                 // section 6; not a probability
  int passes_agree, passes_against, passes_none;
  bool reference;               // first or second reference
  bool at_edge;                 // blob cut by mask or frame edge
  Vote last_vote;               // of the last pass closed
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
  void clear_reference();                 // both references
  // Clears tracks and votes; references kept as marks, bound again to the blob
  // at their place without a contrast check.
  void reset();
  void restore_references(ReferenceMark first, std::optional<ReferenceMark> second);
  std::optional<ReferenceMark> reference_mark() const;
  std::optional<ReferenceMark> second_reference_mark() const;
};

}  // namespace pychron::grains
```

Per frame, in order:

1. Segment. Grains as blobs. Low-relief grains in near-matching oil have faint
   edges: segment on gradient of quiet image, per-pixel low quantile of level
   over `segment_window` samples (clamped to 8..64), not one frame. Becke line
   only adds light, so swept line drops out; real line's dark band not yet
   tested, section 12 gate. Frame box-averaged to at most 800 px wide first.
   Sample kept only when its timestamp at least 50 ms after last kept
   (timestamp not advancing: always kept), so ring spans about 2 s at any rate
   from 20 fps up, more below. Components recomputed every third kept sample;
   frames between return last result. No output during warm-up (fewer than 8
   samples since start, `reset()` or frame size change) and on flat scene.
   While warming up pipeline blind: tracks do not age, no fringe, no vote,
   reference reads `Lost`, so a jump on a fast camera ends no track.
   Blobs: interiors filled, below `min_area_px` dropped. Line dwelling longer
   than window (sweep slower than about 4 s) stands outline off body edge near
   turnaround; velocity does not read outline.
2. Track. Match blobs frame to frame by bounding-box overlap (IoU at least
   0.3); stable id per grain, ids from 1, not reused until `reset()`. Id
   survives gap up to `track_gap_frames`. Blob overlapping two tracks, or
   track overlapping two blobs (merge, split): tracks involved end, blobs
   start new ids, since a label earned by one grain is not the other's.
3. Fringe. Resample ring (`ring_px` each side) around outline into radial
   brightness profile, median baseline removed (edge step leaves no residual).
   Signed fringe velocity `v(t)`, positive outward, plus contrast (peak above
   baseline). Velocity is least-squares slope of fringe's distance from
   tracked centroid over last 5 samples, so slow drift harmless and an
   outline a lingering line moves reads as no motion. Blob cut by mask or
   frame edge, or ring mostly outside frame: no measurement, history cleared.
4. Vote. A pass is a run of frames in which `|v_ref| >= min_ref_speed_px_s`
   with one sign; it closes when reference stalls or sign flips. Pass under
   `min_pass_frames` frames gives no vote, nor does a grain seen in fewer
   frames of it. Per pass, per grain: `Same` or `Opposite` from sign of
   fringe displacement times reference's displacement over the pass (so a
   reversal of focus direction cannot change a vote); `None` when mean
   contrast below `min_contrast` or product zero. Vote weight is grain's mean
   contrast over reference's, at most 1. Reference itself, grains cut by edge
   and grains not measured this frame do not vote.
5. Classify. Section 6.

Global frame shift (whole-frame displacement against previous frame) above
`max_shift_px`: pass in progress dropped, no vote from it, and segmenter and
fringe histories cleared, since nothing before the jump lines up with after.
Also no votes while `camera too slow` (section 9).

Rule: comparison with reference at same moment is only use of z. No encoder,
no focus model, no assumed direction of rack.

## 5. Reference

- Operator drops known material in mount, clicks grain, declares side.
  Standard sanidine `Below`, quartz `Above`. Side remembered between sessions.
- Reference refused, with reason, when its contrast below
  `min_reference_contrast`. Near-oil pick fails early.
- Votes relative to reference grain. Re-click rule: same grain clicked again
  keeps votes; changed side inverts `Candidate` and `Not` at once (labels are
  computed from tallies and reference's side, not stored). Other grain: votes
  clear. `clear_reference()` clears votes and both references.
- Reference refused (`ErrorKind::Config`, reason shown in notice) when: no
  grain at point; grain cut by frame edge; no fringe measured on it yet; its
  recent contrast under `min_reference_contrast`; it is already the other
  reference. Contrast taken as mean of last 5 samples.
- Reference mark (point) follows its grain while tracked and not cut by edge.
  `reset()` keeps mark only of reference tracked at that moment (no gap, not
  cut by edge), bound again to blob at its place, no contrast check; any other
  reference dropped, since a grain of other material there inverts every
  label; second goes with first. `restore_references` binds same way for a
  replay. Frame of other size or depth, or blank frame: both references
  dropped, section 9.
- `ReferenceState`, in banner always:
  - `None`: grains outlined grey, no labels, no votes.
  - `Lost`: votes stop, labels greyed and held. Reference in gap (track
    waiting), cut by edge or pipeline blind: `Lost`; tracked again within gap
    recovers by itself. Track ended: only re-click recovers.
  - `MoveFocus`: reference ok, no pass running.
  - `Reading`: passes counted.
  - `Disagree`: section below.
- No `Candidate` or `Not` label exists while no reference has been set. Label
  without sign worthless.
- Wrong side declared inverts every label. App cannot detect alone. Banner
  always shows reference and declared side.
- Optional second reference, needs first, window gives it side opposite of
  declared first. Tallied against first like any grain: expected `Opposite`
  when sides differ, `Same` when equal. When contradicting passes reach
  `disagree_passes` and exceed expected ones: state `Disagree`, no
  `Candidate` or `Not` label is given (grains read `None` label). Held after
  second's track ends, until first reference changes grain or side, the second
  is set again, references clear or `reset()`.

## 6. Label and score

```
score = (agree + 1) / (agree + against + 2)
```

`agree` and `against`: contrast-weighted votes for grain's leading side. `None`
votes add to neither.

Side from reference: `Same` puts grain on reference's side, `Opposite` on
other. `Below` is `Candidate`, `Above` is `Not`.

- Label is `Candidate` or `Not` only when grain has at least `min_passes` voted
  passes (`Same` plus `Opposite`) and `score >= min_score`. Otherwise `Unsure`;
  tied weights also `Unsure`. Defaults: 3, 0.8. Reference grain itself carries
  no label (`reference` flag). Score shown is for grain's leading side.
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
- Click grain: pass history, contrast, why unsure, in side panel.
- Pin: grain selected by click, then button `Mark picked` (toggle); grain
  greyed. Session only; cleared on every pipeline `reset()` (start of
  recording, camera change), since ids restart; `GrainReport::generation`
  tells window, not recorded, not compared.
- Click on grain with reference tool: set reference; side from chooser
  (`below oil`, `above oil`), kept between sessions. Second-reference tool
  uses opposite of declared first side.
- Banner: reference state, reference id, declared side, warning, camera error,
  recording error, last reference refusal.

## 8. Recording and replay

- Toggle on screen. Writes folder: one PGM per frame processed, through
  `vision::FrameRecorder`, manifest `case.toml` `RecordedSource` reads
  (`fixture.hpp`), plus `tracks.jsonl`: first line options (all of
  `GrainOptions`); then per frame `t_ns`, operator events applied before it
  (`set`, `set_second`, `clear`, `restore`, with mark: point and side), and the
  report: state, warning, per grain outline, `v`, contrast, vote, label, score,
  pass counts, reference flag, edge flag. Folder named `grains-<timestamp>`
  under folder chosen on screen. Recording not finished has no `case.toml` and
  does not replay.
- Recording failure (disk full, say): first error kept, frames after it not
  recorded, shown at once as `Shown::recording_error` (banner) from the frame
  it met it on, stays until stop; `stop_recording` returns it. Live labels
  carry on. Destructor of runner finishes an open recording.
- Starting recording calls `GrainPipeline::reset()`: votes restart, references
  tracked at that moment kept (section 5), grain ids restart from 1, pins
  cleared. `Restore` event logged only for marks that survive. Otherwise replay could not
  rebuild state from before first frame.
- `RecordedSource` stamps at grab, frames carry no time: replay gives it clock
  reading timestamps from `tracks.jsonl`, in order.
- `tracks.jsonl` keeps enough to train model later. No model in this spec.
- After writing folder: `pychron::mark_as_user_file` on each user-opened file
  (`tracks.jsonl`, `case.toml`).
- `grains replay <folder>` headless, prints per-grain summary of last frame.
  Same pipeline, options, times and events from `tracks.jsonl`, first
  `Restore` event giving the reference marks. Compares each frame's report to
  the recorded one (numbers within 1e-9, rest exact). Exit status: 0 all
  match, 1 error (no folder, sidecar not parsing or not matching frames), 3 at
  least one frame differs (summary still printed, count on stderr), 2 bad
  usage.
- Rule: replay of recording gives same report as live gave on same frames.

## 9. Failure

- Camera lost: `LiveFeed::Latest::error` reaches worker as `FrameTick::error`,
  shown once per new error, pipeline not called. Last picture greyed under
  reason (as `apps/pychron-ui` `CameraView`), votes stop, labels held greyed.
  Returns by itself on reopen.
- Frame rate under `min_fps` (default 10), measured by pipeline from
  `Frame::timestamp` over last 8 intervals, or one gap over 2 / `min_fps`, so
  replay agrees: warning `camera too slow`, passes not counted.
- Frame size or `pixel_depth` changes (camera reopened), or blank frame:
  both references dropped, pipeline `reset()`, state `None`. If a reference was
  set, warning `camera changed: click the reference again`, held until one is
  set or next `reset()` (recording start: replay starts clean).
- Global shift: pass dropped (section 4).
- Processing behind camera: frames dropped, never queued.
- Bad `grains.toml`: `Result` error shown at start, app does not run on
  defaults silently.

## 10. Config

`grains.toml`, own file, beside binary's working directory or given by
`--config`:

- `[camera]`: maps to `vision::CameraRequest` (backend, device, width, height,
  fps, `[camera.options]`).
- `[pipeline]`: `min_contrast`, `min_reference_contrast`, `min_passes`,
  `min_score`, `track_gap_frames`, `max_shift_px`, `disagree_passes`,
  `min_fps`, `segment_window`, `ring_px`, `min_area_px`, `min_pass_frames`,
  `min_ref_speed_px_s`.

Defaults for all but `[camera]`. Unknown key, wrong type or value out of
range: `Config` error naming key (`validate_options` checks options held in
memory, `load_config` reads file).

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
