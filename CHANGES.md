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
