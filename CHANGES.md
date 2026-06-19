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
