# Changes: Progress Improvements

## What
- Smoother, monotone progress bar during ODF loading
- Realistic ETA calculation based on elapsed time
- Fine-grained sub-progress within model build phases:
  ranks, manuals, stops, couplers and tremulants reported individually
- Typed abort exceptions: `GOLoadAbortedEarly` / `GOLoadAbortedPartial`
  (`src/grandorgue/loader/GOLoadExceptions.h`)
- `LOG_TIMING` / `LOG_GUI_GAP` macros for profiling (off by default;
  enable via `-DGO_PROFILE_ODFLOAD` / `-DGO_GUI_GAP_TRACER`)
- Memory-limit constructor change: passes `cfgMB * 1024 * 1024` (bytes)
  instead of `cfgMB` to the pool (0 = unlimited remains unchanged)

## Why
The progress bar jumped non-monotonically and showed misleading ETAs
for large organs. Abort handling did not distinguish partial from early
aborts, making error recovery harder.

## Known limitations
- ETA accuracy depends on cache and disk variance across machines.
- `LOG_TIMING` / `LOG_GUI_GAP` require a CMake reconfigure to activate.
