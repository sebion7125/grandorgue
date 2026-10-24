/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOHOTPATH_H
#define GOHOTPATH_H

/* GO_HOT marks the steady-state render path (V3 batch decode, polyphase
 * FIR kernels, fader gain loops, per-period stream classification).
 *
 * GCC emits hot-attributed functions into the .text.hot subsection, so
 * the linker groups them together: the ~50 KB of render-loop code packs
 * into ~13 contiguous pages instead of being interleaved with cold
 * loader/analysis code across ~22 (measured 2026-07-17 on the Linux
 * build). Fewer hot code pages means less L2/L3 and iTLB pressure, and
 * fewer physical-page placement dice per process start (the per-file
 * standby-list lottery - see the 2026-07-17 investigation).
 *
 * Deliberately NOT applied to the giant unrolled non-linear fade clones
 * (ProcessNonLinearFade*, ~110 KB) - packing those would bloat the hot
 * region far past its purpose. */
#if defined(__GNUC__)
#define GO_HOT __attribute__((hot))
#else
#define GO_HOT
#endif

/* GO_ALWAYS_INLINE forces the tiny position / sample-vector accessors
 * (ResamplingPosition::Inc/GetIndex/GetFraction, PlaneVec*::Seek/GetPtr)
 * into the target("avx2") FIR kernels.
 *
 * Plain `inline` is only a hint, and native gcc on the SysV ABI REFUSES to
 * inline a default-target (-msse3) accessor into a target("avx2") function -
 * the target-attribute mismatch trips its inlining cost model. The result on
 * the Linux build was 13 out-of-line calls inside the hot stereo resampler;
 * because the SysV ABI has no callee-saved XMM registers, every YMM
 * accumulator then spilled around each call (23 stack spills) and the inner
 * tile loop stayed rolled - ~40% more dynamic instructions per voice-frame
 * than the MinGW/Windows build, whose ABI (10 callee-saved XMM) lets gcc
 * inline them into a clean, unrolled leaf. always_inline overrides the
 * barrier and restores leaf codegen on both toolchains (measured 2026-07-21:
 * 13 calls / 23 spills -> 0 / 4; static size 175 -> 389, matching Windows). */
#if defined(__GNUC__)
#define GO_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define GO_ALWAYS_INLINE inline
#endif

#endif /* GOHOTPATH_H */
