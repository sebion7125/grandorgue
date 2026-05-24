# Changes: Release Scaling (Experimental / WIP)

## What
- Release gain scaled by actual hold time: short staccato notes get a
  quieter release tail rather than full gain
- Per-MIDI-note lookup tables `g0_*_by_midi[]` and `tmax_*_by_midi[]`
  for Chamade stops (Dry / Front / Rear channels)
- Channel detection via `ChannelFromRankName()` / `IsChamade()` heuristics
- `GetOwnerPipe()` chain in `GOSoundProvider` so the release sampler can
  query rank name from the provider
- `CHAMADE_DEBUG` macro (controlled by `ENABLE_CHAMADE_DEBUG=0` in
  `GO_DebugRelease.h`, off by default)
- General pipes fall back to a simple linear ramp (120 ms, g₀=0.1)

## Why
Short staccato notes produced release tails at full gain, causing
unrealistic reverb blasts. The model is fitted to a specific Chamade stop
as a proof of concept.

## Status: EXPERIMENTAL — not upstream-ready
- Lookup table values are hardcoded for one specific organ/stop
- Parameters must be made ODF-readable before general use
- `GetOwnerPipe()` / `GetRank()` chains are debug scaffolding
- Channel heuristics (keyword matching on rank name) are not robust
