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
