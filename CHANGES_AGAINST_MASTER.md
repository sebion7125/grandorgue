Summary of local changes vs. official master branch (English)

Commit purpose
- Consolidate and harden ODF loading progress reporting and logging.
- Convert ad-hoc wxLogMessage timing/debug prints into centralized, build-time-controlled logging macros.
- Improve cooperative user-abort handling for load worker threads (early vs partial abort).
- Remove duplicated panel-local logging and local timing to avoid noisy duplicates and centralize panel load reporting in the controller.

Files changed (short paragraphs explaining what changed and why)

1) src/grandorgue/gui/panels/GOGUIPanel.cpp
- What changed:
  - Removed panel-local timing (wxStopWatch sw_panel) and the panel-local logging (wxLogMessage(... "GUI.Panel.Load" ...)) and corresponding wxLog::FlushActive().
  - Kept the snapshot API (CreateLoadSnapshot / LoadFromSnapshot) but removed deferred log and timing there as well.
- Why:
  - Panel logs duplicated information already provided by the GOOrganController after panel Load() completes. To reduce noise and avoid double-tagged output, the panel no longer logs load timings or names.
  - Timing/profiling instrumentation was removed from the panel to keep logging centralized; profiling can still be enabled via centralized facilities if later desired.

2) src/core/go_defs.h.in
- What changed:
  - Introduced LOG_TIMING(...) and LOG_GUI_GAP(...) macros (cmake template symbols #cmakedefine GO_PROFILE_ODFLOAD and #cmakedefine GO_GUI_GAP_TRACER).
  - Macros expand to wxLogMessage when their respective CMake defines are set, otherwise they are no-ops.
- Why:
  - Centralize control over profiling/tracer logs via CMake/configure instead of ad-hoc compile-time macros scattered in source files.
  - Allows enabling/disabling large sets of timing logs without modifying source files.

3) src/grandorgue/GOBitmapCache.cpp
- What changed:
  - Replaced direct wxLogMessage timing prints with LOG_GUI_GAP(...) calls.
  - Added local fallback includes/definitions to avoid build break before configure (go_defs.h may not be generated yet).
  - Fixed some wxString::Format usages that caused type/format mismatches on the toolchain.
- Why:
  - Make bitmap decoding timing logs controllable with the centralized macros and avoid spamming logs when tracing is disabled.
  - Avoid build failures when configured headers are not present.

4) src/grandorgue/model/GOStop.cpp
- What changed:
  - Replaced direct wxLogMessage timing lines with LOG_TIMING(...).
  - Added local fallback macro to avoid build break.
- Why:
  - Same reason: centralize timing logs and allow compile-time toggle.

5) src/grandorgue/model/GOOrganModel.cpp
- What changed:
  - Replaced multiple "Timing: ModelPhase ..." wxLogMessage calls with LOG_TIMING(...).
  - Added include and local fallback for LOG_* macros in that translation unit.
- Why:
  - Centralize the model-phase timing output and make it controllable via configure flags.

6) src/grandorgue/GOOrganController.cpp
- What changed:
  - Substantial refactor of progress reporting and cache/direct load loops:
    - Replaced many direct timing/wxLogMessage calls with LOG_TIMING / LOG_GUI_GAP.
    - Progress sink reworked: SetProgressSink uses dialog->Update and throws user-abort exceptions for cooperative aborts.
    - Improved mapping between percent-mode and units-mode; defensive clamping to avoid >100% displays.
    - Implemented time-based synthetic unit estimation when loading cache or files (avg_ms * remaining -> frac_time -> syntheticUnit) to show smoother/ETA-based progress.
    - Reworked cache vs direct load handling and how progress is reported to dialog.
    - Removed duplicated panel logging (controller now owns the canonical panel load messages).
    - Added sentinel return for user-cancel ("!") to avoid double logging in caller.
    - Added local fallbacks for LOG_* macros to support building before configure generation.
- Why:
  - Addressed buggy percent-vs-units scaling and ETA inaccuracies.
  - Ensured progress messages reflect the calculated percentages and are monotonic/clamped.
  - Centralized logging and reduced noisy/debug prints; made progress reporting robust across platforms and native dialogs.

7) src/grandorgue/loader/GOLoadThread.h / GOLoadThread.cpp / GOLoadWorker.cpp / src/grandorgue/loader/GOLoadExceptions.h (new)
- What changed:
  - Introduced two new exception classes for controlled aborts: GOLoadAbortedEarly and GOLoadAbortedPartial.
  - GOLoadThread now tracks m_userAbort (atomic) and captures std::exception_ptr. Entry() differentiates between partial/early abort exceptions and other std::exception.
  - GOLoadWorker::LoadObjectNoExc catches exceptions and sets flags instead of letting exceptions propagate.
  - Worker threads cooperatively set WasUserAbort / m_userAbort so the controller can decide partial vs early abort handling.
  - Added small helper ThrowGOLoadAborted* usage where cooperative abort must propagate to outer loops.
- Why:
  - Previously user cancels could cause generic std::exception dialogs and inconsistent behavior. The new design ensures cooperative aborts are reported and handled cleanly, allowing the controller to produce a single "partial load" warning (or early abort sentinel) rather than multiple error dialogs.
  - Makes thread exception reporting robust and prevents stray unhandled dialogs in UI.

8) src/grandorgue/GOBitmapCache.cpp (additional)
- What changed:
  - Fixed format-usage and replaced direct wxLogMessage occurrences with the macros.
- Why:
  - Avoid toolchain-specific format mismatches and make logging centrally configurable.

9) Other loader/model files
- What changed:
  - Scattered replacements of wxLogMessage(...) timing lines to LOG_TIMING/LOG_GUI_GAP across the codebase (GOStop, GOOrganModel, GOOrganController, etc.).
  - Defensive fixes for format strings and minor API usage (e.g. safe index usage).
- Why:
  - Incremental efforts to remove noisy timing logs and route them through centralized macros so output is only present when enabled.

Problems addressed
- Duplicate panel logs: removed in panel; controller provides canonical panel load logging.
- Percent-vs-units scaling errors: implemented strict detection (max==100 → percent; otherwise units) and time-based synthetic unit estimation.
- Stray std::exception dialogs on user aborts: introduced typed abort exceptions and cooperative thread flagging so aborts are handled gracefully.
- Excessive/uncontrolled timing logs: centralized via LOG_TIMING and LOG_GUI_GAP with go_defs.h configure-time control.
- Build fragility prior to configure: added local no-op fallbacks in source files where necessary to avoid breakage before go_defs.h is generated.

Pending tasks
- Implement LOG_FLUSH in go_defs.h.in (optional) so tracing macros flush output immediately when tracing is enabled (recommended for live logs).
- Run smoke tests: normal load, early abort, partial abort on target platforms (Windows & Linux) to verify dialog and log behavior.
- Review other files for remaining direct wxLogMessage usages and consolidate similarly.
- Optionally, remove temporary per-file local fallback definitions once build+configure is consistent.

Current state / what I did immediately before this summary
- I removed the panel-local timing & duplicate logging from src/grandorgue/gui/panels/GOGUIPanel.cpp and committed the change (commit a6f05e7a).
- I already converted many timing logs to LOG_TIMING / LOG_GUI_GAP and added local fallbacks to avoid breakage pre-configure.
- I ran incremental builds between edits and verified successful compilation.

Suggested next step (direct and aligned with your instruction)
- Add a documentation commit that enumerates these local modifications relative to the official master branch (I can create docs/CHANGES_AGAINST_MASTER.md and commit it). This will make review and code audit easier.
- Run the smoke tests and provide log excerpts showing the controller-only panel logs (no duplication) and correct progress behavior.

Quoted context (recent user instruction)
- "Perfekt. Du müsstest in deinem Kontext ja die wichtigsten infos haben... Mache nun einen Commit mit aussagekräftiger beschreibung, was gegenüber dem online master branch von GO hier geändert wurde. Schreibe zu jeder datei einen kurzen absatz ... All in English."

</context>
</summarize_task>
<task_progress>
- [x] Analysis & summary prepared
- [ ] Finalize and commit the summary document (if requested)
</task_progress>
