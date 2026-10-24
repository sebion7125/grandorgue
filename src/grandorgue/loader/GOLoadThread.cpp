/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOLoadThread.h"

#include "loader/GOLoadExceptions.h"
#include "model/GOCacheObject.h"

#include <wx/log.h>

bool GOLoadThread::CheckExceptions() {
  // Wait for thread completion or stop.
  Wait();

  // If the thread captured an exception during Entry(), report it as an
  // exception condition so the caller inspects GetException().
  try {
    if (HasException())
      return true;
  } catch (...) {
    // In case HasException() itself throws (shouldn't), fall through and
    // ensure we return true to signal an exception occurred.
    return true;
  }

  // Fall back to legacy worker exception reporting which may throw
  // GOOutOfMemory() or return the worker exception flag.
  try {
    return WereExceptions();
  } catch (...) {
    // Capture any exception thrown during WereExceptions() so caller can
    // rethrow/inspect via GetException() if desired.
    m_exception = std::current_exception();
    return true;
  }
}

void GOLoadThread::Entry() {
  try {
    GOCacheObject *obj = nullptr;
    while (!ShouldStop() && LoadNextObject(obj)) {
      // loop body intentionally empty; LoadNextObject does the work
      // and handles its own per-object exceptions where possible.
    }
  } catch (const GOLoadAbortedPartial &) {
    // Cooperative user abort observed in worker thread (partial abort during
    // cache/WAV). Mark the flag so main thread can detect a user abort without
    // producing a noisy "std::exception" log entry.
    m_userAbort.store(true);
  } catch (const GOLoadAbortedEarly &) {
    // Early aborts observed inside worker are unexpected but should be treated
    // as a user abort to avoid noisy std::exception diagnostics.
    m_userAbort.store(true);
  } catch (const std::exception &e) {
    // Log concrete exception message for diagnostics and capture it so main
    // can rethrow in its context.
    wxLogError("Worker uncaught std::exception: %s", e.what());
    m_exception = std::current_exception();
  } catch (...) {
    // Unknown exception type
    wxLogError("Worker uncaught unknown exception");
    m_exception = std::current_exception();
  }
}
