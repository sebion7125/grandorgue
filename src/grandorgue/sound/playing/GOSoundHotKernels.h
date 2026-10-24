/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDHOTKERNELS_H
#define GOSOUNDHOTKERNELS_H

/* Explicit-instantiation roster for the hot AVX2 render kernels.
 *
 * The kernels (V3 batch decode, polyphase FIR) are member templates.
 * Implicitly instantiated they become COMDAT sections, and on PE/MinGW a
 * COMDAT loses the hot attribute's placement: the linker sorts it by
 * MANGLED NAME into the monolithic .text, far away from the .text.hot
 * block that the non-template hot code (GOSoundStream/GOSoundFader ~28 KB)
 * correctly reaches. The kernels then neighbour arbitrary cold code - and
 * WHICH code that is reshuffles with every build. Measured 2026-07-22/23:
 * a reproducible per-BUILD voice-cost spread of ~5% on the 46 GB organ at
 * 128-sample buffers (memory-bound regime: scattered hot code costs extra
 * iTLB entries and L2 code refetches that compete with the streaming
 * sample data), identical across byte-copies of one binary but different
 * between builds.
 *
 * This header declares every kernel instantiation the engine uses
 * (extern template - suppresses the COMDAT copies in the two using TUs,
 * GOSoundStream.cpp and GOSoundGroupTask.cpp). GOSoundHotKernels.cpp
 * flips GO_KERNEL_TPL to definitions via GO_HOT_KERNELS_DEFINE: every
 * kernel then lives in that single TU's .text.hot, contiguously and IN
 * THIS DECLARATION ORDER. Keep the order matching the per-voice render
 * pipeline (batch decode -> FIR planes/accumulate) so code that runs
 * together sits on the same few pages. The classic-window FIR variants
 * cannot live here (the window classes are defined inside
 * GOSoundStream.cpp) - they are explicitly instantiated at the end of
 * that TU instead, joining ITS .text.hot block.
 *
 * Standalone tools (compression benches etc.) that include the kernel
 * headers but not this one keep their own implicit instantiations - do
 * NOT include this header outside golib. */

#include "GOSoundCompressionCacheV3Batch.h"
#include "GOSoundStream.h"

#ifdef GO_HOT_KERNELS_DEFINE
#define GO_KERNEL_TPL template
#else
#define GO_KERNEL_TPL extern template
#endif

#ifdef GO_V3_BATCH_AVX2
/* --- stage 1: V3 batch decode (the biggest kernels, ~16 KB each) --- */
GO_KERNEL_TPL void GOSoundCompressionCacheV3Batch::DecodeBatchImpl<1>(
  GOSoundCompressionCacheV3Batch::Lane[],
  unsigned,
  unsigned,
  int32_t *const[][MAX_OUTPUT_CHANNELS],
  int32_t *const[MAX_OUTPUT_CHANNELS],
  unsigned,
  GOSoundCompressionCacheV3 *,
  unsigned);
GO_KERNEL_TPL void GOSoundCompressionCacheV3Batch::DecodeBatchImpl<2>(
  GOSoundCompressionCacheV3Batch::Lane[],
  unsigned,
  unsigned,
  int32_t *const[][MAX_OUTPUT_CHANNELS],
  int32_t *const[MAX_OUTPUT_CHANNELS],
  unsigned,
  GOSoundCompressionCacheV3 *,
  unsigned);
#endif // GO_V3_BATCH_AVX2

#ifdef GO_POLYPHASE_AVX2
/* --- stage 2: FIR on the batch path's planar planes (+ accumulate) --- */
GO_KERNEL_TPL void
GOSoundResample::PolyphaseResampler::ResampleBlockAvx2<
  GOSoundStream::PlaneVecStereo,
  2>(GOSoundResample::ResamplingPosition &, GOSoundStream::PlaneVecStereo &, float *, unsigned)
  const;
GO_KERNEL_TPL void
GOSoundResample::PolyphaseResampler::ResampleBlockAvx2<
  GOSoundStream::PlaneVecMono,
  2>(GOSoundResample::ResamplingPosition &, GOSoundStream::PlaneVecMono &, float *, unsigned)
  const;
GO_KERNEL_TPL void
GOSoundResample::PolyphaseResampler::ResampleBlockAvx2Accum<
  GOSoundStream::PlaneVecStereo,
  2>(GOSoundResample::ResamplingPosition &, GOSoundStream::PlaneVecStereo &, float *, unsigned, float, float)
  const;
GO_KERNEL_TPL void
GOSoundResample::PolyphaseResampler::ResampleBlockAvx2Accum<
  GOSoundStream::PlaneVecMono,
  2>(GOSoundResample::ResamplingPosition &, GOSoundStream::PlaneVecMono &, float *, unsigned, float, float)
  const;

#endif // GO_POLYPHASE_AVX2

#undef GO_KERNEL_TPL

#endif /* GOSOUNDHOTKERNELS_H */
