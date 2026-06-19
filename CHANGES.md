# Changes: Memory Limit Warning

## What
- Live warning label in Settings → Options → Memory Limit spinner
- Shows percentage of total system RAM; turns red above 85%
- New handlers: `OnMemoryLimitSpin` / `OnMemoryLimitText` / `UpdateMemoryLimitWarning`

## Why
Users could set a memory limit above available RAM without any feedback,
leading to OOM crashes when loading large organs.

## Files changed
- `src/grandorgue/gui/dialogs/settings/GOSettingsOptions.cpp`
- `src/grandorgue/gui/dialogs/settings/GOSettingsOptions.h`

---

# Changes: Polyphony Load Dropping

## What
- Panic mode triggers earlier: threshold reduced from 172×16 to 2000 samples
- New soft-drop path for releases at ≥80% of polyphony soft limit
  (fade-out over ~1000 ms with ±500 ms jitter)
- Randomized fade-out times via `rand()` in both paths to prevent
  concurrent voice drops from synchronizing and causing audible artifacts

## Why
The previous deterministic drop timing caused periodic audible glitches
when many voices hit the polyphony limit at the same time.
The earlier panic threshold improves responsiveness under overload.

## Files changed
- `src/grandorgue/sound/GOSoundOrganEngine.cpp` (ProcessSampler)

---

# Changes: Progress Improvements

## What
- Smoother, monotone progress bar during ODF loading
- Fine-grained sub-progress within model build phases:
  ranks, manuals, stops, couplers and tremulants reported individually
- Time-based ETA estimation during audio cache loading
- Typed abort exceptions: `GOLoadAbortedEarly` / `GOLoadAbortedPartial`
- Progress now uses `GOProgressMonitor` interface (decoupled from GUI)
- `LOG_TIMING` / `LOG_GUI_GAP` macros for profiling (off by default)

## Why
The progress bar jumped non-monotonically and showed no progress during
the model build phase, which can take up to half the total load time on
large organs. Cancellation was also not possible during that phase.

## Known limitations
- ETA accuracy during audio loading is still noisy — improvement planned.

---

# Changes: Crossfade Curves

## What
- 5 selectable crossfade curves: Linear, SinEqualPower, Sin², SqrtEqualPower, X²
- Central evaluator `go_crossfade_eval(GOCrossfadeMode, float t) → {a, b}`
  in `GOCrossfadeMode.h`
- LUT cache (`FastCrossfadeCache`) in `fast_crossfade.h` for non-linear fades
  without per-sample trig/sqrt in the hot path
- Stepper-based recurrent sin computation for inner loops
- Crossfade mode selector in Audio menu (keyboard shortcuts F7–F12)
- CrossfadeMode persisted per organ in `.cmb`; defaults to Linear for old files
- Loop crossfade: `kForceLegacyLoopCrossfade=true` preserves pre-existing
  cached cosine behaviour
- Fused Fade+Accumulate path: `ProcessAndAccumulate()` (opt-in via
  `GO_ENABLE_FUSED_FADE_ACCUMULATE` or `GOAudioParams::GetFuseFadeAndAccumulate()`)
- Template precomputation in `GOSoundOrganEngine::Setup()` for common buffer sizes

## Why
The original crossfade was a fixed cosine curve hardwired at build time.
Users wanted perceptually equal-power alternatives and runtime switching.

## Known limitations
- Custom mode is a placeholder (returns `{0, 1}`)
- Loop crossfade mode switching not yet active (`kForceLegacyLoopCrossfade=true`)
- Fused accumulate path is opt-in and off by default
