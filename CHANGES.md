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
