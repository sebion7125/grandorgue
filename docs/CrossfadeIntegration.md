Crossfade mode integration — detailed report
===========================================

Purpose
-------
The goal of the change was to centralize the gain calculation for Sustain → Release and loop crossfades and keep backward compatibility. A central evaluation function was introduced/used (go_crossfade_eval) and applied at relevant hotspots. At the same time, the old (cached) loop crossfade behavior remains unchanged until a deliberate migration strategy is chosen.

Summary of changes
------------------
- Central evaluation API: go_crossfade_eval(GOCrossfadeMode mode, float t) returning GOCrossfadeGains { float a, b }.
- Loop crossfade (cached loops) in GOSoundAudioSection::DoCrossfade:
  - Prepared to use go_crossfade_eval(mode, t).
  - Legacy behavior (cosine-based factor) preserved behind a constant to keep existing cache data compatible.
- Runtime fader (GOSoundFader): linear fast-path preserved; non-linear curves use go_crossfade_eval.
- Release gain calculation when creating detached release samplers (GOSoundEngine::CreateReleaseSampler) switched to go_crossfade_eval.
- GUI / persistence:
  - CrossfadeMode is written to the .cmb when exporting settings.
  - When loading: if the .cmb has no CrossfadeMode entry (older organs / first load without .cmb), CrossfadeMode is explicitly set to Linear to preserve legacy behavior.
- Comment cleanup: German comments in actively edited files were translated to English.

Files changed (key list)
------------------------
- src/grandorgue/sound/GOCrossfadeMode.h
  - GOCrossfadeMode enum
  - inline GOCrossfadeGains go_crossfade_eval(GOCrossfadeMode m, float t)
- src/grandorgue/sound/GOCrossfadeParam.h
  - Thread-safe global CrossfadeMode (std::atomic<uint8_t>)
  - GetCrossfadeMode() / SetCrossfadeMode()
- src/grandorgue/sound/GOSoundAudioSection.cpp
  - DoCrossfade(...) adjusted to compute t and either use the legacy cosine factor (kept as a constant) or go_crossfade_eval
- src/grandorgue/sound/GOSoundFader.h / GOSoundFader.cpp
  - Process(...) keeps the linear fast path
  - ProcessNonLinearFade(...) (previously ProcessSinusFade) uses go_crossfade_eval for non-linear modes
- src/grandorgue/sound/GOSoundEngine.cpp
  - CreateReleaseSampler(): attack/release gain_delta computation → go_crossfade_eval
- src/grandorgue/gui/frames/GOFrame.cpp
  - Menu mapping: GetCrossfadeMode() → Check(ID_Crossfade_..., true)
- src/grandorgue/GOOrganController.cpp
  - ReadOrganFile(): when CrossfadeMode entry is missing, set Linear (backwards compatibility)
- src/grandorgue/sound/GO_Attack_Parameters.h
  - Comments translated to English

Key code excerpts
-----------------
1) Central evaluator (GOCrossfadeMode.h)
```cpp
struct GOCrossfadeGains { float a, b; };

inline GOCrossfadeGains go_crossfade_eval(GOCrossfadeMode m, float t) {
  if (t < 0.f) t = 0.f; else if (t > 1.f) t = 1.f;
  switch (m) {
    case GOCrossfadeMode::Linear:        return {1.f - t, t};
    case GOCrossfadeMode::SinEqualPower: return {std::cos(kGOCrossfadeHalfPi*t), std::sin(kGOCrossfadeHalfPi*t)};
    case GOCrossfadeMode::Sin2:          { float a = std::cos(kGOCrossfadeHalfPi*t); a *= a; float b = std::sin(kGOCrossfadeHalfPi*t); b *= b; return {a,b}; }
    case GOCrossfadeMode::SqrtEqualPower:return {std::sqrt(1.f - t), std::sqrt(t)};
    case GOCrossfadeMode::X2:            return {1.f - t*t, t*t};
    case GOCrossfadeMode::Custom:        return {0.f, 1.f}; // placeholder
  }
  return {1.f - t, t};
}
```

2) DoCrossfade — core change (simplified)
```cpp
// constant to keep old cached loop behaviour until we choose to migrate caches
static constexpr bool kForceLegacyLoopCrossfade = true;

for (unsigned pos = 0; pos < fade_length; pos++) {
  float t = float(pos + 0.5f) / float(fade_length);
  if (kForceLegacyLoopCrossfade) {
    float factor = (cos(M_PI * (pos + 0.5) / fade_length) + 1.0) * 0.5f;
    result = val1 * factor + val2 * (1.0f - factor);
  } else {
    using namespace GOAudioParams;
    const auto mode = GetCrossfadeMode();
    const auto g = go_crossfade_eval(mode, t);
    result = g.a * val1 + g.b * val2;
  }
}
```

3) Fader: non-linear branch using the same evaluator
```cpp
const auto mode = GetCrossfadeMode();
const auto g = go_crossfade_eval(mode, x); // x in [0..1]
if (increasing) fadeFactor = g.b;
else if (decreasing) fadeFactor = g.a;
```

Design rationale
----------------
- Central evaluator reduces duplication, ensures consistent behavior between loop crossfades and the runtime fader, and simplifies adding new crossfade curves.
- Preserve legacy behavior because caches hold precomputed/cached loop transition data; silently changing that would create audible differences and could break cache assumptions. The constant kForceLegacyLoopCrossfade (currently true) keeps the legacy path active until a migration is planned.
- GUI/persistence:
  - The menu maps enum → menu id (robust, not relying on sequential IDs).
  - CrossfadeMode is persisted in .cmb; for older .cmb files lacking the value, the loader sets Linear explicitly (historical default).

Compatibility implications
--------------------------
- Old .cmb files without CrossfadeMode: explicitly set to Linear (legacy behavior preserved).
- Cache hashes do not include CrossfadeMode (GenerateCacheHash focuses on structural/audio data). Nevertheless, we retain legacy loop crossfade behavior to be safe.
- If kForceLegacyLoopCrossfade is disabled in the future, plan a migration path:
  - detect cached loop format on load and select legacy path accordingly, or
  - regenerate caches (UpdateCache) for affected organs.

Threading & performance
-----------------------
- GetCrossfadeMode() reads a std::atomic<uint8_t> — thread-safe.
- Linear path avoids expensive trig operations and is kept as the fast path.
- Non-linear modes (sin/cos/sqrt) are only used when selected.

Recommended tests / verification
--------------------------------
Minimum test plan:
- Load an old organ without .cmb → GUI shows Linear and sound matches legacy behavior.
- Load a new organ with CrossfadeMode set → GUI reflects mode and loop crossfades follow that mode.
- Switch CrossfadeMode at runtime while fades are active → check for audible artifacts.
- Smoke test: play loops with varying crossfade lengths & modes; measure CPU usage.
- Unit test: verify go_crossfade_eval for t = 0, 0.5, 1.0 for each enum value.
- Cache test: create cache with legacy behavior; load with kForceLegacyLoopCrossfade = true and false to observe effects.

Next steps / recommendations
----------------------------
1. Decide on kForceLegacyLoopCrossfade:
   - Keep it constant (safe), or
   - Replace with cache-header versioning / a flag, or
   - Add a runtime option in GOAudioParams + GUI to control migration strategy.
2. Add a short developer note in GOSoundAudioSection::DoCrossfade explaining the cache compatibility rationale (there is already a comment — expand if needed).
3. Optionally extract a small helper to map enum → menu ID to centralize GUI code (refactor GOFrame).
4. Optionally run a full build + smoke tests and add unit tests for go_crossfade_eval.

If you want, I can:
- convert this into a PR description or checklist, or
- scan the repo for any remaining German comments and translate them, or
- implement a cache‑version flag for safe migration.
