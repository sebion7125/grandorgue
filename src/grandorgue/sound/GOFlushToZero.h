/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOFLUSHTOZERO_H
#define GOFLUSHTOZERO_H

/* Denormal handling for the render threads.
 *
 * The build enables -ffast-math (CMakeLists.txt), which links crtfastmath.o
 * and sets the SSE flush-to-zero / denormals-are-zero MXCSR bits - but ONLY
 * on the process's initial thread. MXCSR is per-thread, and on Linux/glibc a
 * pthread starts with the default MXCSR (denormals ENABLED); it does NOT
 * inherit the main thread's FTZ/DAZ. So without this, the sound worker
 * threads - where the actual DSP runs - process denormals the slow way
 * (microcode assist, 10-100x per op) whenever a voice decays into tiny
 * values (release tails, crossfade lanes, near-silent leftovers). Windows
 * thread startup happened to differ, which is one reason the same build
 * measured slower per sample on Linux.
 *
 * Call this once at the start of every thread that runs render/mix code
 * (the worker pool and the audio callback thread). */

/* MXCSR denormal-control bits (for the read-back diagnostic below). */
#define GO_MXCSR_FTZ 0x8000u // flush-to-zero (bit 15)
#define GO_MXCSR_DAZ 0x0040u // denormals-are-zero (bit 6)

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64)              \
  || defined(_M_IX86)
#include <pmmintrin.h> // _MM_SET_DENORMALS_ZERO_MODE (SSE3)
#include <xmmintrin.h> // _MM_SET_FLUSH_ZERO_MODE (SSE)

static inline void GOEnableFlushToZero() {
  _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
  _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
}

/* Raw MXCSR of the calling thread - read this BEFORE enabling FTZ/DAZ to
 * see the mode the OS/runtime actually handed the thread (differs between
 * platforms and thread-creation paths). Bit 0x8000 = FTZ, 0x0040 = DAZ. */
static inline unsigned GOReadMxcsr() { return _mm_getcsr(); }
#else
static inline void GOEnableFlushToZero() {}
static inline unsigned GOReadMxcsr() { return 0; }
#endif

#endif /* GOFLUSHTOZERO_H */
