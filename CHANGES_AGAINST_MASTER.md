Summary of local changes vs. official master branch (English)

Overview
- Goal: Improve ODF loading UX and diagnostics while keeping functional behavior unchanged for normal usage. Specifically, produce reliable, monotonic progress reporting and useful ETA during large loads; centralize and gate verbose timing/log output behind configure-time macros so profiling output can be enabled for debugging without spamming normal runs; and implement cooperative, typed aborts: Eearly arbort closes the organ and later arbort leads to a partial load as in the past versions.
- Scope: UI progress reporting, logging/tracing macros(#cmakedefine GO_PROFILE_ODFLOAD
#cmakedefine GO_GUI_GAP_TRACER), loader thread abort semantics, and defensive formatting/usage fixes. No intentional changes to runtime audio behavior or loading semantics outside of progress reporting and abort handling.

Purpose
- This file lists only the source files that differ between the current local branch and the repository's local `master` branch, with a short, review-oriented paragraph per file describing the meaningful change(s) relative to master. It omits transient experimental notes and internal-only workflow edits.

Files changed (concise per-file paragraphs)

src/grandorgue/GOOrganController.cpp / src/grandorgue/GOOrganController.h
- Reworked progress reporting for ODF/cache/direct loads: strict detection of percent vs. units modes, defensive clamping, and time‑based synthetic‑unit estimation to produce smoother, monotonic percent values and more realistic ETA. Progress sink semantics were adjusted so dialog->Update triggers cooperative abort handling.

src/grandorgue/gui/dialogs/GOProgressDialog.cpp / GOProgressDialog.h
- Improved Update/ResetRange handling so the progress message includes a calculated percent prefix and percent-vs-units mapping is applied consistently. Small UI defensive fixes (monotonic updates, clamping) to avoid 100%+ or regressing progress values.


src/core/go_defs.h.in
- Added configure-time logging macros (LOG_TIMING, LOG_GUI_GAP) controlled by CMake defines to centralize and gate timing/tracing output. This allows enabling detailed timing logs without scattering ad-hoc wxLogMessage calls across the tree.

src/grandorgue/GOBitmapCache.cpp
- Replaced direct timing wxLogMessage calls with the centralized LOG_GUI_GAP macro and fixed a few format-string issues so builds are robust across toolchains. The change centralizes bitmap-decoding timing logs under the new tracing macro.

src/grandorgue/loader/GOLoadExceptions.h
- Added typed exceptions for cooperative aborts (GOLoadAbortedEarly, GOLoadAbortedPartial). These allow the loader thread/controller to distinguish early aborts (user canceled before significant work) from partial aborts (stop during cache/deserialization), preventing stray generic error dialogs and enabling a single, clear UI message. User-facing effect: users can cancel loading immediately at any time — early aborts are treated as cancellation before substantial work, partial aborts stop during cache/deserialization — and the controller reports a single clear UI message rather than surfacing generic exception dialogs.

src/grandorgue/loader/GOLoadThread.cpp / GOLoadThread.h / GOLoadWorker.cpp
- Thread/worker error-path hardening: worker code captures exceptions into std::exception_ptr for the controller to inspect and uses atomic flags for WasUserAbort to coordinate cooperative cancellations. Worker-level exception propagation was clarified so the controller can decide whether to treat a stop as early or partial.

src/grandorgue/model/GOOrganModel.cpp
- Replaced noisy ad-hoc timing prints with LOG_TIMING macros where appropriate and added small defensive fixes. Timing output is now controlled by the configure-time macro instead of unguarded wxLogMessage calls.

src/grandorgue/model/GOStop.cpp
- Converted a few model-phase timing logs to LOG_TIMING and added defensive guards; no functional change to model behavior, only centralized and controllable timing output.

Notes
- This changelog intentionally excludes internal experimental notes, local-only workflow changes, and transient files. It focuses strictly on the source files that contain changes meaningful to reviewers of the code differences versus the local `master` branch.
- For reproducible review, run: git diff --name-only master..HEAD to verify the exact changed file list used to compose this summary.
