/*
 * Load-related exception types and small helpers.
 * Two distinct exceptions are used so the controller can distinguish
 * early aborts (before audio load) from partial aborts (during WAV/cache).
 */

#ifndef GOLOAD_EXCEPTIONS_H
#define GOLOAD_EXCEPTIONS_H

#include <exception>

class GOLoadAbortedEarly : public std::exception {};
class GOLoadAbortedPartial : public std::exception {};

// Diagnostic helpers that log origin and throw the appropriate exception.
static inline void ThrowGOLoadAbortedEarly(const char * /*where*/) {
  throw GOLoadAbortedEarly();
}

static inline void ThrowGOLoadAbortedPartial(const char * /*where*/) {
  throw GOLoadAbortedPartial();
}

#endif
