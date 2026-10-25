/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundGroupTask.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <wx/log.h>

#include "sound/GOSoundOrganEngine.h"
#include "sound/playing/GOSoundAudioSection.h"
#include "sound/playing/GOSoundHotKernels.h"
#include "sound/playing/GOSoundResampleBatch.h"
#include "sound/playing/GOSoundStream.h"
#include "sound/playing/GOSoundSubblockGrid.h"
#include "threading/GOMutexLocker.h"

#include "GOSoundWindchestTask.h"

/* Fused-path render variant (2026-07-12). 1 = variant D: decode stays
 * interleaved (lockstep-native), one 8x8 transpose flips the group span
 * planar, every lane renders through the CLASSIC per-voice path
 * (contiguous taps + the production fader/filter chain in
 * AccumulateSamplerBuffer). Replaces the gather FIR - measured 2.90 vs
 * 4.85 ns/voice-frame on identical warm data
 * (tools/compression/fir_batch_vs_planar_bench.cpp). 0 = the previous
 * gather kernel (RenderAccumulate) incl. its gainBuf/laneOut special
 * paths, kept intact for A/B comparison. */
#define GO_V3_TRANSPOSE_RENDER 1
/* In-situ stage profiler (2026-07-13): per-group rdtsc split of the fused
 * path into decode / FIR / fader+accumulate / admin, aggregated as
 * cumulative cycle counters and shown in the Audio Monitor. ~7 ns/voice
 * overhead - diagnosis tool, set to 0 for clean measurements. */
#define GO_STAGE_PROF 0
/* Steady-constant gain+accumulate fused into the FIR loop: tried
 * 2026-07-13 and DISABLED the same night - the ceiling DROPPED below
 * 19.2k. Root cause: the two-pass form accumulates 8-wide
 * (accumConstAvx2 over the 1 KB temp), the fused form degraded that to
 * 256 scalar read-modify-writes inside the FIR loop - wrong fusion
 * granularity; the saved temp round-trip does not pay for losing the
 * vector accumulate. A correct fusion would need vector-tile output
 * (e.g. 4-frame hsum batching + vector RMW) - future option. */
// RenderBlockFromPlanesAccum (GOSoundStream.h) hard-depends on the
// AVX2-only ResampleBlockAccum (GOSoundResample.h), unguarded itself -
// found 2026-10-25 building on ARM (aarch64/armhf), where
// GO_POLYPHASE_AVX2 is correctly undefined but this flag still enabled
// the call site. Tying it to GO_POLYPHASE_AVX2 directly falls back to
// the always-present classic two-pass path below on any non-x86 build.
#ifdef GO_POLYPHASE_AVX2
#define GO_V3_FUSED_CONST_GAIN 1
#else
#define GO_V3_FUSED_CONST_GAIN 0
#endif
#if GO_STAGE_PROF
#include <x86intrin.h>
#endif
/* Within variant D: 1 = the decoder stores planar directly (transpose
 * fused into its store stage - bench F2, zero extra registers: the
 * decoded value rows reuse the just-consumed residual rows' slots);
 * 0 = decode interleaved + separate TransposeToPlanes pass (F1). */
#define GO_V3_DECODE_PLANAR 1

GOSoundGroupTask::GOSoundGroupTask(
  GOSoundOrganEngine &sound_engine, unsigned samples_per_buffer)
  : GOSoundBufferTaskBase(2, samples_per_buffer),
    m_engine(sound_engine),
    m_Condition(m_Mutex),
    m_ActiveCount(0),
    m_Done(0),
    m_Stop(false) {}

void GOSoundGroupTask::Reset() {
  GOMutexLocker locker(m_Mutex);

  /* Never reset while a drain is in flight (state 4): the drainer works
   * without the mutex, and resetting to 0 here would elect a SECOND
   * drainer that races the first on the epoch arrays. Bounded wait: the
   * publish broadcast arrives within a fraction of a millisecond. */
  while (m_Done.load() == 4)
    m_Condition.WaitOrStop("GOSoundGroupTask::Reset");
  m_Done.store(0);
  // m_ActiveCount is intentionally NOT zeroed here: every participant
  // increments it in Run.beforeProcess and decrements it in Run.afterProcess
  // (both under m_Mutex), so it is balanced by construction. Zeroing it
  // while a delayed worker was still inside its processing loop turned that
  // worker's later fetch_sub into an unsigned wrap (0 -> UINT_MAX), making
  // the "last thread" check in Run.afterProcess fire prematurely for the
  // *next* period - periods then overlapped and the first thread's
  // clear()+refill of the shared flat arrays raced the straggler, orphaning
  // samplers (the "silent voices never end" bug).
  m_Stop.store(false);
}

void GOSoundGroupTask::Clear() {
  // Only called with no participants inside Run() (see WaitAndClear), so
  // the persistent structures may be dropped wholesale. The samplers
  // themselves are owned by the engine's pool - same as the old design,
  // which also just forgot its lists here.
  for (PersistentVoiceSet *set : {&m_ActiveSet, &m_ReleaseSet}) {
    set->inbox.Clear();
    set->buckets.clear();
    set->prefix.clear();
    set->total.store(0, std::memory_order_relaxed);
    set->staleSeen.store(0, std::memory_order_relaxed);
  }

  // Exhausted cursors of the current generation: nothing claimable until
  // the next period window opens a new generation.
  const uint64_t closed
    = ((uint64_t)m_Generation.load(std::memory_order_relaxed) << 32)
    | 0xFFFFFFFFull;

  m_ActiveCursor.store(closed, std::memory_order_relaxed);
  m_ReleaseCursor.store(closed, std::memory_order_relaxed);
}

void GOSoundGroupTask::Add(GOSoundSampler *sampler) {
  if (sampler->is_release)
    m_ReleaseSet.inbox.Put(sampler);
  else
    m_ActiveSet.inbox.Put(sampler);
}

/* Simple per-voice decode diagnostics (implementation plan step 13:
 * counters before any performance claim). Plain cumulative atomics bumped
 * by ProcessVoices; read by the Audio Monitor (GOAudioMonitorDlg), which
 * shows per-interval rates and carries them into the audio report.
 *
 * One cacheline each (2026-07-27). Declared back to back these packed
 * eight per line, and all 20 workers issue fetch_add on them during every
 * batch and every group flush - so the counters that merely observe the
 * hot path were bouncing its cachelines between all cores, a cost that
 * grows with worker count and exists only because of the diagnostics.
 * The padding is ~700 bytes of BSS in exchange. Reads (once per monitor
 * tick) do not care about the layout. */
alignas(64) static std::atomic<uint64_t> s_Rebuilds{0}; // voices rebuilt this run
alignas(64) static std::atomic<uint64_t> s_BatchCalls{0};  // BuildBlocksBatch calls with >=2 voices (AVX2-eligible)
alignas(64) static std::atomic<uint64_t> s_SoloRebuilds{0}; // single-voice rebuilds (always scalar)
// Voices through the fused decode+resample+fade path (Phase 2) - the
// success metric of the batch renderer: at high polyphony this should
// approach rebuilds, leaving BuildOnly/classic as the small remainder.
alignas(64) static std::atomic<uint64_t> s_BatchRendered{0};
// Per-period flat-list sizes (one sample per non-empty active/release list
// per period per task) - the sizing basis for any period-end claim tuning:
// how much tail a straggler batch can hold depends on how long the lists
// actually are, which varies per organ (voices spread over group tasks).
alignas(64) static std::atomic<uint64_t> s_ListSamples{0};
alignas(64) static std::atomic<uint64_t> s_ListVoicesSum{0};
alignas(64) static std::atomic<uint64_t> s_ListSizeMax{0}; // window max, reset on read
// Callback-help policy instrumentation (see GOSoundOrganEngine::
// MayCallbackHelp): how often the audio thread participated in voice
// work via Finish, how often the policy denied it, how many µs the
// period's first thread spent in the (lock-free) drain, and how many
// Exec calls found their period still unfinished (the "Exec broom"
// running leftovers synchronously on the RT thread).
alignas(64) static std::atomic<uint64_t> s_CbHelpRuns{0};
alignas(64) static std::atomic<uint64_t> s_CbHelpDenied{0};
alignas(64) static std::atomic<uint64_t> s_DrainUsSum{0};
alignas(64) static std::atomic<uint64_t> s_ExecLatePeriods{0};
// Voices whose processing the AUDIO THREAD itself performed (via Finish
// help or the Exec broom) - numerator of the monitor's "CB work" curve.
alignas(64) static std::atomic<uint64_t> s_CbVoices{0};
#if GO_STAGE_PROF
// Stage cycles of the fused render path (see GO_STAGE_PROF).
alignas(64) static std::atomic<uint64_t> s_StDecodeCy{0};
alignas(64) static std::atomic<uint64_t> s_StFirCy{0};
alignas(64) static std::atomic<uint64_t> s_StFaderCy{0};
alignas(64) static std::atomic<uint64_t> s_StFusedCy{0}; // steady-const fused kernel
alignas(64) static std::atomic<uint64_t> s_StAdminCy{0}; // group remainder
#endif

GOSoundGroupTask::DecodeStats GOSoundGroupTask::GetDecodeStats() {
  return {
    s_Rebuilds.load(std::memory_order_relaxed),
    s_BatchCalls.load(std::memory_order_relaxed),
    s_SoloRebuilds.load(std::memory_order_relaxed),
    s_BatchRendered.load(std::memory_order_relaxed),
    GOSoundStream::s_SyncFallbacks.load(std::memory_order_relaxed),
    s_ListSamples.load(std::memory_order_relaxed),
    s_ListVoicesSum.load(std::memory_order_relaxed),
    // Window max: the monitor is the only reader; each poll starts a new
    // observation window so a one-off long list stays visible for exactly
    // one poll instead of forever.
    s_ListSizeMax.exchange(0, std::memory_order_relaxed),
    s_CbVoices.load(std::memory_order_relaxed),
    s_CbHelpRuns.load(std::memory_order_relaxed),
    s_CbHelpDenied.load(std::memory_order_relaxed),
    s_DrainUsSum.load(std::memory_order_relaxed),
    s_ExecLatePeriods.load(std::memory_order_relaxed),
#if GO_STAGE_PROF
    s_StDecodeCy.load(std::memory_order_relaxed),
    s_StFirCy.load(std::memory_order_relaxed),
    s_StFaderCy.load(std::memory_order_relaxed),
    s_StFusedCy.load(std::memory_order_relaxed),
    s_StAdminCy.load(std::memory_order_relaxed)
#else
    0, 0, 0, 0, 0
#endif
  };
}

void GOSoundGroupTask::ProcessVoices(
  GOSoundSampler *const bufs[],
  unsigned count,
  GOSoundSampler *const nextBufs[],
  unsigned nextCount,
  bool toDropOld,
  float *output_buffer,
  unsigned prefetchAhead) {
  const unsigned nFrames = GetNFrames();

  // Classic per-voice render for a sampler that already passed
  // ProcessSamplerPreRender: exactly ProcessSampler's middle + tail.
  auto renderClassic
    = [&](GOSoundSampler *sampler, float volume, bool wasProcessed) {
        if (wasProcessed) {
          float temp[nFrames * 2];

          if (!sampler->stream.ReadBlock(temp, nFrames))
            sampler->p_SoundProvider = NULL;
          m_engine.AccumulateSamplerBuffer(
            output_buffer, sampler, nFrames, temp, volume);
        }
        // Persistent buckets: NO re-Add. A kept voice simply stays
        // resident; a leaving one had its residence token invalidated
        // inside PostRender/ReturnSampler (lazy removal).
        m_engine.ProcessSamplerPostRender(sampler, wasProcessed);
      };

#ifdef GO_RESAMPLE_BATCH_AVX2
  const bool batchRenderOk = GOSoundResampleBatch::CpuSupported()
    && GOSoundStream::IsSubblockCacheEnabled()
    && GOSoundSubblockBins::GetBase() == nFrames;
  constexpr unsigned MAX_GROUP = GOSoundResampleBatch::LANES;

  // Pending fused render group: same (channels, bin-rung) key rule as
  // the former decode pass - the decode kernel steps its lanes in
  // lockstep. The flat array is bucketed by that key (DrainIntoEpoch),
  // so key-change flushes are rare bucket seams.
  GOSoundSampler *rPend[MAX_GROUP];
  unsigned rStarts[MAX_GROUP];
  unsigned rValids[MAX_GROUP];
  unsigned nR = 0;
  // Pending build-only group (blocks built into the voices' own
  // VoiceBlocks, rendered classically right after).
  GOSoundSampler *bPend[MAX_GROUP];
  unsigned bStarts[MAX_GROUP];
  unsigned bValids[MAX_GROUP];
  unsigned nB = 0;
  unsigned groupChannels = 0;
  unsigned groupBin = 0;
  unsigned groupOrder = 0;
  /* Read the mode flag ONCE per render block, not per voice: it is an
   * atomic the compiler may not hoist out of the voice loop itself, and at
   * 24k voices that load is pure per-voice overhead the fixed-order path
   * must not pay. The mode is fixed for the session (it is folded into the
   * cache hash, so a change means a reload). */
  const bool sortByOrder = GOSoundAudioSection::IsVariablePredictorOrder();

  // Scratch for one fused group, reused across flushes. Decode scratch
  // is bounded by ClassifyForRender (BATCH_RENDER_MAX_TOTAL); render/
  // lane-out scratch by the engine buffer size. VLA-sized to the real
  // configuration (the codebase already uses VLAs on this path).
  /* Lane span capacity, rounded up to a multiple of 8 so the transpose
   * pass's whole-tile loop always covers the decoded span (see
   * TransposeToPlanes; only relevant for odd samples_per_buffer). */
  const unsigned laneSpanCap
    = (std::min(
         GOSoundSubblockBins::GetMaxBin() + GOSoundStream::GUARD_SAMPLES,
         GOSoundStream::BATCH_RENDER_MAX_TOTAL)
       + 7u)
    & ~7u;
  const unsigned decodeScratchLen = laneSpanCap * MAX_GROUP;
  const unsigned laneOutLen = nFrames * MAX_GROUP;
  // Flat VLAs with fixed-size pointer tables: gcc cannot capture
  // variably-modified 2D arrays in lambdas, pointers it can.
  int32_t decodeScratchStore[MAX_OUTPUT_CHANNELS * decodeScratchLen];
  float laneOutStore[MAX_OUTPUT_CHANNELS * laneOutLen];
  int32_t *const scratch[MAX_OUTPUT_CHANNELS]
    = {decodeScratchStore, decodeScratchStore + decodeScratchLen};
  // Planar destination of the variant-D transpose pass (lane l's run at
  // planes[c] + l*laneSpanCap); same footprint as the decode scratch.
  int32_t planeStore[MAX_OUTPUT_CHANNELS * decodeScratchLen];
  int32_t *const planes[MAX_OUTPUT_CHANNELS]
    = {planeStore, planeStore + decodeScratchLen};
  float *const laneOut[MAX_OUTPUT_CHANNELS]
    = {laneOutStore, laneOutStore + laneOutLen};
  // Per-frame gain buffer for fused groups containing non-linear
  // crossfades (kernel variant (b), gains[frame*LANES+lane]); filled and
  // passed only when such a lane exists - see flushRender.
  float gainBufStore[laneOutLen];
  // Per-frame gains of the group's ToneBalance lanes (same layout),
  // consumed by the FilterFadeAccumulate pass - see flushRender.
  float filterGainStore[laneOutLen];

  /* RawFused lanes (uncompressed sections, 2026-07-13): no decoder, so
   * there is no group decode to share - build the voice's own planar
   * block inline (raw widen) and render it through the same fused
   * kernels as the V3 lanes. Previously these voices ran a scalar FIR
   * over PtrSampleVector plus the two-pass fader (42% of Rotterdam's
   * voice calls). */
  auto renderRawFused
    = [&](GOSoundSampler *sampler, unsigned start, unsigned valid) {
        GOSoundStream &stream = sampler->stream;
        const unsigned channels = stream.GetSectionChannels();

        stream.BuildBlockScalar(start, valid);

        const float volume = sampler->p_WindchestTask->GetVolume();
        const int32_t *plane0 = stream.GetBlockChannelData(0);
        const int32_t *plane1
          = channels > 1 ? stream.GetBlockChannelData(1) : plane0;

#if GO_V3_FUSED_CONST_GAIN
        float steadyGain0, steadyDGain;

        if (
          !sampler->toneBalanceFilterState.IsToApply()
          && sampler->fader.TrySteadyLinearGain(
            volume, nFrames, steadyGain0, steadyDGain)) {
          stream.RenderBlockFromPlanesAccum(
            plane0,
            plane1,
            start,
            channels,
            output_buffer,
            nFrames,
            steadyGain0,
            steadyDGain);
          m_engine.CountFaderKernel(
            steadyDGain == 0.0f ? GOFadeKernelKind::Constant
                                : GOFadeKernelKind::Ramp);
          m_engine.ProcessSamplerPostRender(sampler, true);
          return;
        }
#endif // GO_V3_FUSED_CONST_GAIN

        float temp[nFrames * 2];

        stream.RenderBlockFromPlanes(
          plane0, plane1, start, channels, temp, nFrames);
        m_engine.AccumulateSamplerBuffer(
          output_buffer, sampler, nFrames, temp, volume);
        // Persistent-buckets semantics: NO re-Add (see flushRender).
        m_engine.ProcessSamplerPostRender(sampler, true);
      };

  auto flushRender = [&]() {
    if (!nR)
      return;
    if (nR == 1) {
      // A lone voice gains nothing from the 8-lane kernels (they always
      // pay full SIMD cost): build its block classically and render it
      // through the normal window path.
      GOSoundStream *stream = &rPend[0]->stream;

      GOSoundStream::BuildBlocksBatch(
        &stream, rStarts, rValids, 1, groupChannels);
      s_Rebuilds.fetch_add(1, std::memory_order_relaxed);
      s_SoloRebuilds.fetch_add(1, std::memory_order_relaxed);
      renderClassic(
        rPend[0], rPend[0]->p_WindchestTask->GetVolume(), true);
      nR = 0;
      return;
    }

#if GO_V3_TRANSPOSE_RENDER
    {
      /* Variant D: batch-decode (unchanged, interleaved), flip the span
       * planar once, then render every lane through the classic
       * per-voice path. All the special-lane machinery of the gather
       * kernel below (gainBuf columns, laneOut detours, the vectorized
       * ToneBalance pass) collapses into AccumulateSamplerBuffer, which
       * already handles every fader kernel and the filter - and counts
       * the voice-mix statistics. */
      GOSoundStream *streamsD[MAX_GROUP];
      unsigned maxValid = 0;

      for (unsigned l = 0; l < nR; l++) {
        streamsD[l] = &rPend[l]->stream;
        maxValid = std::max(maxValid, rValids[l]);
      }
#if GO_STAGE_PROF
      const uint64_t tGroup0 = __rdtsc();
#endif
#if GO_V3_DECODE_PLANAR
      GOSoundStream::DecodeGroupToPlanes(
        streamsD, rStarts, rValids, nR, groupChannels, planes, laneSpanCap);
#else
      GOSoundStream::DecodeGroupToScratch(
        streamsD, rStarts, rValids, nR, groupChannels, scratch);
      GOSoundResampleBatch::TransposeToPlanes(
        scratch,
        groupChannels,
        (maxValid + GOSoundStream::GUARD_SAMPLES + 7u) & ~7u,
        planes,
        laneSpanCap);
#endif
      (void)maxValid;

      s_Rebuilds.fetch_add(nR, std::memory_order_relaxed);
      s_BatchCalls.fetch_add(1, std::memory_order_relaxed);
      s_BatchRendered.fetch_add(nR, std::memory_order_relaxed);
#if GO_STAGE_PROF
      const uint64_t tDecodeEnd = __rdtsc();
      uint64_t firCy = 0, faderCy = 0, fusedCy = 0;

      s_StDecodeCy.fetch_add(tDecodeEnd - tGroup0, std::memory_order_relaxed);
#endif
      for (unsigned l = 0; l < nR; l++) {
        GOSoundSampler *const sampler = rPend[l];
        const float volume = sampler->p_WindchestTask->GetVolume();
#if GO_V3_FUSED_CONST_GAIN
        float steadyGain0, steadyDGain;

        if (
          /* ToneBalance voices must keep the two-pass path - the fused
           * kernel would silently skip the filter (latent since the
           * const-gain fusion; never hit in practice: the monitor showed
           * 0% filter voices, but the gate belongs here). */
          !sampler->toneBalanceFilterState.IsToApply()
          && sampler->fader.TrySteadyLinearGain(
            volume, nFrames, steadyGain0, steadyDGain)) {
#if GO_STAGE_PROF
          const uint64_t tF0 = __rdtsc();
#endif

          /* Steady-linear fast path (stage profiler 2026-07-12: the
           * two-pass fader stage was 17% of the fused path at 19k held
           * voices): fuse gain+accumulate into the FIR loop - no temp
           * round-trip, no fader pass. Covers steady-constant (dg == 0,
           * bit-identical to the former constant-only fusion) and the
           * external-volume smoothing ramp (dg != 0). */
          sampler->stream.RenderBlockFromPlanesAccum(
            planes[0] + (size_t)l * laneSpanCap,
            (groupChannels > 1 ? planes[1] : planes[0])
              + (size_t)l * laneSpanCap,
            rStarts[l],
            groupChannels,
            output_buffer,
            nFrames,
            steadyGain0,
            steadyDGain);
#if GO_STAGE_PROF
          fusedCy += __rdtsc() - tF0;
#endif
          m_engine.CountFaderKernel(
            steadyDGain == 0.0f ? GOFadeKernelKind::Constant
                                : GOFadeKernelKind::Ramp);
          m_engine.ProcessSamplerPostRender(sampler, true);
          continue;
        }
#endif // GO_V3_FUSED_CONST_GAIN

        float temp[nFrames * 2];

#if GO_STAGE_PROF
        const uint64_t tL0 = __rdtsc();
#endif
        sampler->stream.RenderBlockFromPlanes(
          planes[0] + (size_t)l * laneSpanCap,
          (groupChannels > 1 ? planes[1] : planes[0])
            + (size_t)l * laneSpanCap,
          rStarts[l],
          groupChannels,
          temp,
          nFrames);
        // RenderBlockFromPlanes advanced the position classically - no
        // AdvanceAfterBatchRender here.
#if GO_STAGE_PROF
        const uint64_t tL1 = __rdtsc();
#endif
        m_engine.AccumulateSamplerBuffer(
          output_buffer, sampler, nFrames, temp, volume);
#if GO_STAGE_PROF
        const uint64_t tL2 = __rdtsc();

        firCy += tL1 - tL0;
        faderCy += tL2 - tL1;
#endif
        /* Persistent-buckets semantics: NO re-Add - the voice stays
         * resident, departures happen via token invalidation. COUPLING
         * WARNING: on a pre-buckets scheduler this line MUST become
         * `if (...PostRender(...)) Add(sampler);` - building this render
         * variant onto the old scheduler without that change orphans
         * every fused voice after one period (found the hard way,
         * 2026-07-11 22:39: 'sustained gap / orphaned sampler leak',
         * plus a fake 21k polyphony display of silent zombie samplers). */
        m_engine.ProcessSamplerPostRender(sampler, true);
      }
#if GO_STAGE_PROF
      s_StFirCy.fetch_add(firCy, std::memory_order_relaxed);
      s_StFaderCy.fetch_add(faderCy, std::memory_order_relaxed);
      s_StFusedCy.fetch_add(fusedCy, std::memory_order_relaxed);
      s_StAdminCy.fetch_add(
        (__rdtsc() - tDecodeEnd) - firCy - faderCy - fusedCy,
        std::memory_order_relaxed);
#endif
      nR = 0;
      return;
    }
#endif // GO_V3_TRANSPOSE_RENDER

    GOSoundStream *streams[MAX_GROUP];
    GOSoundResampleBatch::Lane lanes[MAX_GROUP];
    bool needsClassicFader[MAX_GROUP];
    bool laneGainFilled[MAX_GROUP];
    bool laneFilter[MAX_GROUP];
    bool anyClassicFader = false;
    bool anyFilterLane = false;
    bool useGainBuf = false;

    for (unsigned l = 0; l < nR; l++) {
      GOSoundSampler *const sampler = rPend[l];
      GOSoundStream &stream = sampler->stream;
      const float volume = sampler->p_WindchestTask->GetVolume();

      streams[l] = &stream;
      lanes[l].rel = 0; // the block begins at this callback's first read
      lanes[l].fraction = stream.GetResamplingFraction();
      lanes[l].increment = stream.GetResamplingIncrement();
      lanes[l].g0 = 0.0f;
      lanes[l].dg = 0.0f;
      laneGainFilled[l] = false;
      laneFilter[l] = false;
      needsClassicFader[l] = false;

      const bool needsFilter = sampler->toneBalanceFilterState.IsToApply();

      /* A ToneBalance lane renders in the batch with zero gain (its raw
       * samples land in laneOut) and gets filter+fade+accumulate applied
       * by the vectorized FilterFadeAccumulate pass below - its per-frame
       * gains go into filterGainStore. It stays classic only when its
       * fader needs the template-less stepper fallback. */
      if (needsFilter) {
        bool gainsKnown;

        if (sampler->fader.NeedsNonLinearProcessing()) {
          gainsKnown = sampler->fader.FillFrameGains(
            nFrames, volume, &filterGainStore[l], GOSoundResampleBatch::LANES);
          if (gainsKnown)
            m_engine.CountFaderKernel(GOFadeKernelKind::NonLinearFade);
        } else {
          float g0, dg;

          m_engine.CountFaderKernel(
            sampler->fader.ComputeFrameGains(nFrames, volume, g0, dg));
          // Same iterative ramp as fader.Process would apply.
          for (unsigned j = 0; j < nFrames; j++) {
            filterGainStore[(size_t)j * GOSoundResampleBatch::LANES + l] = g0;
            g0 += dg;
          }
          gainsKnown = true;
        }
        if (gainsKnown) {
          laneFilter[l] = true;
          anyFilterLane = true;
        } else {
          needsClassicFader[l] = true;
          anyClassicFader = true;
        }
        continue;
      }

      if (sampler->fader.NeedsNonLinearProcessing()) {
        /* Non-linear crossfade mode: fill this lane's per-frame gain
         * column (kernel variant (b) - the gainBuf input wired through
         * in Phase 2 exactly for this). Covers active template fades
         * AND the no-envelope constant/ramp tail, so under Sin2 etc.
         * the whole group stays on the fused SIMD path. Previously
         * EVERY voice took the laneOut + scalar-fader detour whenever
         * the global mode was non-linear - which made a mass release
         * (all voices entering their fade in the same callback) fall
         * off the SIMD path at once: the audible "release all keys"
         * glitch. */
        laneGainFilled[l] = sampler->fader.FillFrameGains(
          nFrames, volume, &gainBufStore[l], GOSoundResampleBatch::LANES);
        if (laneGainFilled[l]) {
          useGainBuf = true;
          m_engine.CountFaderKernel(GOFadeKernelKind::NonLinearFade);
        }
        // else: an active envelope without a bound template (stepper-only
        // fallback) - route through the classic path below.
      }
      needsClassicFader[l]
        = !laneGainFilled[l] && sampler->fader.NeedsNonLinearProcessing();
      if (needsClassicFader[l])
        // Rendered in the batch, faded classically from laneOut; its
        // batch-accumulate contribution is neutralized via zero gain.
        anyClassicFader = true;
      else if (!laneGainFilled[l])
        m_engine.CountFaderKernel(
          sampler->fader.ComputeFrameGains(
            nFrames, volume, lanes[l].g0, lanes[l].dg));
    }
    if (useGainBuf)
      /* With gainBuf active the kernel reads EVERY lane's gain from the
       * buffer - fill the remaining columns. */
      for (unsigned l = 0; l < GOSoundResampleBatch::LANES; l++) {
        if (l < nR && laneGainFilled[l])
          continue;
        if (l < nR && !needsClassicFader[l]) {
          /* Linear lane in a gain-buffered group: replicate the kernel's
           * own iterative ramp (use g, then g += dg) so moving the group
           * onto the buffer changes nothing bit-wise for this voice. */
          float g = lanes[l].g0;

          for (unsigned j = 0; j < nFrames; j++) {
            gainBufStore[(size_t)j * GOSoundResampleBatch::LANES + l] = g;
            g += lanes[l].dg;
          }
        } else
          // Classic-detour and padded lanes must contribute nothing;
          // their columns would otherwise be uninitialized stack.
          for (unsigned j = 0; j < nFrames; j++)
            gainBufStore[(size_t)j * GOSoundResampleBatch::LANES + l] = 0.0f;
      }
    GOSoundStream::DecodeGroupToScratch(
      streams, rStarts, rValids, nR, groupChannels, scratch);
    GOSoundResampleBatch::RenderAccumulate(
      lanes,
      nR,
      scratch,
      groupChannels,
      nFrames,
      streams[0]->GetResample()->m_PolyphaseCoefs,
      output_buffer,
      (anyClassicFader || anyFilterLane) ? laneOut : nullptr,
      useGainBuf ? gainBufStore : nullptr);
    if (anyFilterLane) {
      /* Vectorized ToneBalance: one filter+fade+accumulate pass over the
       * group's filter lanes. Non-participating lanes are inert via zero
       * coefficients, state and gain columns. */
      alignas(32) double b0[GOSoundResampleBatch::LANES] = {0};
      alignas(32) double b1[GOSoundResampleBatch::LANES] = {0};
      alignas(32) double a1[GOSoundResampleBatch::LANES] = {0};
      alignas(32) double st[MAX_OUTPUT_CHANNELS][GOSoundResampleBatch::LANES]
        = {{0}};

      for (unsigned l = 0; l < GOSoundResampleBatch::LANES; l++) {
        if (l < nR && laneFilter[l]) {
          float stF[2];

          rPend[l]->toneBalanceFilterState.ExportBatch(
            b0[l], b1[l], a1[l], stF);
          st[0][l] = stF[0];
          st[1][l] = stF[1];
        } else
          for (unsigned j = 0; j < nFrames; j++)
            filterGainStore[(size_t)j * GOSoundResampleBatch::LANES + l]
              = 0.0f;
      }
      GOSoundResampleBatch::FilterFadeAccumulate(
        b0,
        b1,
        a1,
        st,
        laneOut,
        filterGainStore,
        groupChannels,
        nFrames,
        output_buffer);
      for (unsigned l = 0; l < nR; l++)
        if (laneFilter[l]) {
          /* Mono groups only advance channel 0 in the pass; mirror it so
           * a later classic-path takeover sees the same state in both
           * channels (the classic detour filters a duplicated buffer,
           * whose two identical chains stay in lockstep). */
          const float stF[2] = {
            (float)st[0][l],
            (float)(groupChannels > 1 ? st[1][l] : st[0][l])};

          rPend[l]->toneBalanceFilterState.ImportBatchState(stF);
        }
    }
    s_Rebuilds.fetch_add(nR, std::memory_order_relaxed);
    s_BatchCalls.fetch_add(1, std::memory_order_relaxed);
    s_BatchRendered.fetch_add(nR, std::memory_order_relaxed);
    for (unsigned l = 0; l < nR; l++) {
      GOSoundSampler *const sampler = rPend[l];

      sampler->stream.AdvanceAfterBatchRender(nFrames);
      if (needsClassicFader[l]) {
        // De-interleave this lane's raw resampled samples (tiny,
        // L1-warm) and send them through the unchanged single-voice
        // fader/ToneBalance chain.
        float temp[nFrames * 2];

        for (unsigned j = 0; j < nFrames; j++) {
          const float ch0
            = laneOut[0][(size_t)j * GOSoundResampleBatch::LANES + l];

          temp[j * 2] = ch0;
          temp[j * 2 + 1] = groupChannels > 1
            ? laneOut[1][(size_t)j * GOSoundResampleBatch::LANES + l]
            : ch0;
        }
        m_engine.AccumulateSamplerBuffer(
          output_buffer,
          sampler,
          nFrames,
          temp,
          sampler->p_WindchestTask->GetVolume());
      }
      m_engine.ProcessSamplerPostRender(sampler, true); // stays resident
    }
    nR = 0;
  };
#endif // GO_RESAMPLE_BATCH_AVX2

  auto flushBuild = [&]() {
#ifdef GO_RESAMPLE_BATCH_AVX2
    if (!nB)
      return;

    GOSoundStream *streams[MAX_GROUP];

    for (unsigned l = 0; l < nB; l++)
      streams[l] = &bPend[l]->stream;
    GOSoundStream::BuildBlocksBatch(
      streams, bStarts, bValids, nB, groupChannels);
    s_Rebuilds.fetch_add(nB, std::memory_order_relaxed);
    if (nB >= 2)
      s_BatchCalls.fetch_add(1, std::memory_order_relaxed);
    else
      s_SoloRebuilds.fetch_add(1, std::memory_order_relaxed);
    for (unsigned l = 0; l < nB; l++)
      renderClassic(bPend[l], bPend[l]->p_WindchestTask->GetVolume(), true);
    nB = 0;
#endif
  };

  for (unsigned i = 0; i < count; i++) {
    if (prefetchAhead > 0) {
      /* Two-stage prefetch pipeline (2026-07-12). Stage 1 (far, raw): the
       * sampler STRUCT of the voice 2*prefetchAhead away - no deref, just
       * its first cache lines (hot control fields + stream head). Stage 2
       * below (near) calls GetCurrentPtr() on the voice prefetchAhead
       * away; that call itself walks the struct and the block-end state,
       * which stage 1 made resident - previously those dependent loads
       * stalled inside the prefetch helper itself, ~one cold line per
       * voice per period across a multi-MB pool. */
      const unsigned ahead2 = i + 2 * prefetchAhead;
      const GOSoundSampler *far = nullptr;

      if (ahead2 < count)
        far = bufs[ahead2];
      else if (ahead2 - count < nextCount)
        far = nextBufs[ahead2 - count];
      if (far) {
        const char *base = reinterpret_cast<const char *>(far);

        __builtin_prefetch(base, 0, 1);
        __builtin_prefetch(base + 64, 0, 1);
        __builtin_prefetch(base + 128, 0, 1);
        /* The render also touches lines deep inside the struct that the
         * three header lines above do not cover: the dependent-forward
         * decoder state (PrepareBlockState reads it first thing in the
         * group decode) and the fader head (AccumulateSamplerBuffer's
         * mode/volume fields). offsetof on this non-standard-layout
         * struct is conditionally-supported and fine on gcc/clang/msvc. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
        constexpr size_t kStreamOff = offsetof(GOSoundSampler, stream);
        constexpr size_t kFaderOff = offsetof(GOSoundSampler, fader);
#pragma GCC diagnostic pop
        __builtin_prefetch(base + kStreamOff + 64, 0, 1);
        __builtin_prefetch(base + kFaderOff, 0, 1);
      }

      const unsigned ahead = i + prefetchAhead;
      const unsigned char *ptr = nullptr;
      if (ahead < count)
        ptr = bufs[ahead]->stream.GetCurrentPtr();
      else if (ahead - count < nextCount)
        ptr = nextBufs[ahead - count]->stream.GetCurrentPtr();
      if (ptr) {
        /* Prefetch the voice's FULL expected consumption, not just its
         * first cacheline: one render period eats ~300-500 bytes of
         * compressed data (~1-1.5 bytes/sample/channel at 128 frames
         * stereo), i.e. 6-8 lines - a single-line hint left ~85% of the
         * span cold and the decode kernel stalling on it (measured IPC
         * 0.55 at full load; voices scatter across a multi-GB sample set,
         * so every span starts DRAM-cold). 512 bytes is a deliberate
         * over-estimate: prefetch hints cannot fault and stale extra
         * lines are cheaper than a cold stall. Runtime A/B unchanged:
         * PrefetchDistance 0 (Audio Monitor spinner) disables all of it. */
        constexpr unsigned PREFETCH_SPAN_BYTES = 512;

        for (unsigned off = 0; off < PREFETCH_SPAN_BYTES; off += 64)
          __builtin_prefetch(ptr + off, 0, 1);
      }
    }
    GOSoundSampler *const sampler = bufs[i];
    if (
      toDropOld && m_Stop.load()
      && sampler->time + 2000 < m_engine.GetTime()) {
      if (sampler->drop_counter++ > 3) {
        m_engine.ReturnSampler(sampler);
        continue;
      }
    }
    sampler->drop_counter = 0;
    GOSoundWindchestTask *const windchest = sampler->p_WindchestTask;
    if (!windchest)
      continue; // matches the old loop: no windchest, no processing

    const bool due = m_engine.ProcessSamplerPreRender(sampler);

    if (!due) {
      // Not yet due (delay) - no render; the voice stays resident (or
      // leaves via PostRender's token invalidation).
      m_engine.ProcessSamplerPostRender(sampler, false);
      continue;
    }

#ifdef GO_RESAMPLE_BATCH_AVX2
    unsigned start;
    unsigned valid;
    const GOSoundStream::RenderClass cls = batchRenderOk
      ? sampler->stream.ClassifyForRender(start, valid)
      : (sampler->stream.NeedsBlockRebuild(start, valid)
           ? GOSoundStream::RenderClass::BuildOnly
           : GOSoundStream::RenderClass::None);

    if (cls == GOSoundStream::RenderClass::RawFused) {
      // Independent of the V3 group state: no flush, no bin coupling.
      renderRawFused(sampler, start, valid);
      continue;
    }
    if (cls != GOSoundStream::RenderClass::None) {
      const unsigned channels = sampler->stream.GetSectionChannels();
      const unsigned bin = sampler->stream.GetBinSamples();
      // The batch decoder dispatches ONE hardcoded predictor per 8-lane
      // group, so in Variable mode a group must stay uniform-order: flush
      // on order change. In the fixed modes all orders match, so this is
      // gated off to keep the single-order path unchanged. (The bucket-key
      // order fold already makes same-order voices contiguous, so these
      // flushes are rare bucket seams, not per-voice churn.)
      const unsigned order
        = sortByOrder ? sampler->stream.GetPredictorOrder() : 0;
      if (
        (nR > 0 || nB > 0)
        && (channels != groupChannels || bin != groupBin
            || order != groupOrder)) {
        flushRender();
        flushBuild();
      }
      groupChannels = channels;
      groupBin = bin;
      groupOrder = order;
      if (cls == GOSoundStream::RenderClass::BatchRender) {
        rPend[nR] = sampler;
        rStarts[nR] = start;
        rValids[nR] = valid;
        if (++nR == MAX_GROUP)
          flushRender();
      } else {
        bPend[nB] = sampler;
        bStarts[nB] = start;
        bValids[nB] = valid;
        if (++nB == MAX_GROUP)
          flushBuild();
      }
      continue;
    }
#else
    {
      // No batch kernels on this platform: prebuild via the scalar path
      // when needed, then render classically.
      unsigned start;
      unsigned valid;

      if (sampler->stream.NeedsBlockRebuild(start, valid)) {
        GOSoundStream *stream = &sampler->stream;

        GOSoundStream::BuildBlocksBatch(
          &stream, &start, &valid, 1, sampler->stream.GetSectionChannels());
        s_Rebuilds.fetch_add(1, std::memory_order_relaxed);
        s_SoloRebuilds.fetch_add(1, std::memory_order_relaxed);
      }
    }
#endif
    renderClassic(sampler, windchest->GetVolume(), true);
  }
#ifdef GO_RESAMPLE_BATCH_AVX2
  flushRender();
#endif
  flushBuild();
}

/* Globally unique, never-reused residence tokens (see the header's
 * PersistentVoiceSet comment). Starts at 1; 0 means "resident nowhere". */
static std::atomic<uint64_t> s_ResidenceTokenCounter{0};

unsigned GOSoundGroupTask::ProcessList(
  PersistentVoiceSet &set,
  std::atomic<uint64_t> &cursor,
  uint32_t myGen,
  bool toDropOld,
  float *output_buffer,
  bool &hasLoot) {
  // Double-buffered batch acquisition: while processing cur[], the next batch
  // nxt[] is already held so prefetch hints can reach across the batch seam.
  const int prefetchRaw = m_engine.GetPrefetchDistance();
  const unsigned prefetchAhead = prefetchRaw > 0 ? (unsigned)prefetchRaw : 0;

  GOSoundSampler *bufs[2][VOICE_BATCH_SIZE];
  unsigned liveCounts[2] = {0, 0};
  unsigned cur = 0;
  unsigned staleLocal = 0;

  /* Claims one range via the gen-checked CAS cursor and copies its LIVE
   * entries out (stale ones - token mismatch, the voice left - are
   * skipped and counted). Returns the claimed range length; 0 means
   * nothing is left for myGen (period exhausted, or the cursor moved on
   * to another generation - a stale claim never consumes anything). */
  auto acquireBatch = [&](unsigned slot) -> unsigned {
    liveCounts[slot] = 0;

    uint64_t curVal = cursor.load(std::memory_order_relaxed);
    unsigned start = 0;
    unsigned n = 0;

    for (;;) {
      if ((uint32_t)(curVal >> 32) != myGen)
        return 0;
      const unsigned pos = (uint32_t)curVal;
      const unsigned total = set.total.load(std::memory_order_relaxed);

      if (pos >= total)
        return 0;
      const unsigned remaining = total - pos;

      /* LAST-PHASE tail split (2026-07-13). History: guided claim sizing
       * over the last ~256 voices was tried (c790e581e) and reverted
       * (b319fd2e7) - too broad, moderate results. Per the user's
       * directive this now triggers ONLY in the very last phase
       * (remaining <= two batches): the final full batch would otherwise
       * serialize the period's completion on one worker (~13 us at 128)
       * while the rest idle. Never below 8 - smaller claims cannot feed
       * the 8-lane fused kernels and would push the tail onto the solo
       * path (the reverted attempt's documented lesson). */
      unsigned batch = VOICE_BATCH_SIZE;

      if (remaining <= VOICE_BATCH_SIZE * 2)
        batch = std::max(8u, remaining / 4);
      n = std::min(batch, remaining);
      if (cursor.compare_exchange_weak(
            curVal, curVal + n, std::memory_order_acq_rel)) {
        start = pos;
        break;
      }
      n = 0;
    }

    /* Copy [start, start+n) of the virtual bucket concatenation. Safe
     * without further synchronization: the vectors are only mutated in
     * the period window, which cannot begin while this participant is
     * registered (m_ActiveCount), and the successful CAS above proves we
     * belong to the current generation. */
    const std::vector<unsigned> &prefix = set.prefix;
    unsigned k = (unsigned)(std::upper_bound(prefix.begin(), prefix.end(), start)
                            - prefix.begin())
      - 1;
    unsigned live = 0;
    const unsigned end = start + n;

    for (unsigned i = start; i < end;) {
      while (prefix[k + 1] <= i)
        k++;
      const std::vector<PersistentVoiceSet::Entry> &bucket = set.buckets[k];
      const unsigned off = i - prefix[k];
      const unsigned take = std::min(end, prefix[k + 1]) - i;

      for (unsigned j = 0; j < take; j++) {
        const PersistentVoiceSet::Entry &e = bucket[off + j];

        if (
          e.sampler->m_ResidenceToken.load(std::memory_order_relaxed)
          == e.token)
          bufs[slot][live++] = e.sampler;
        else
          staleLocal++;
      }
      i += take;
    }
    liveCounts[slot] = live;
    return n;
  };

  unsigned claimed = acquireBatch(cur);
  unsigned totalProcessed = 0;

  while (claimed > 0) {
    const unsigned nxt = 1 - cur;
    const unsigned nextClaimed = acquireBatch(nxt);

    if (liveCounts[cur] > 0) {
      if (!hasLoot) {
        /* First live batch of this participation: zero the accumulator
         * (voices only ever ADD into it). Deferred to this point so a
         * participant that claims nothing never touches the buffer. */
        std::fill_n(output_buffer, 2 * GetNFrames(), 0.0f);
        hasLoot = true;
      }
      ProcessVoices(
        bufs[cur],
        liveCounts[cur],
        bufs[nxt],
        liveCounts[nxt],
        toDropOld,
        output_buffer,
        prefetchAhead);
      totalProcessed += liveCounts[cur];
    }
    cur = nxt;
    claimed = nextClaimed;
  }
  if (staleLocal)
    set.staleSeen.fetch_add(staleLocal, std::memory_order_relaxed);
  return totalProcessed;
}

void GOSoundGroupTask::ApplyPeriodWindow(PersistentVoiceSet &set) {
  bool changed = false;

  /* Compaction: a stale entry burns claim-copy work every period (it is
   * re-skipped once per period until removed), so fold them away once
   * enough accumulated - amortized O(changes), and zero cost in steady
   * state. */
  const unsigned stale = set.staleSeen.load(std::memory_order_relaxed);
  const unsigned total = set.total.load(std::memory_order_relaxed);

  if (stale > 32 && stale * 8 > total) {
    for (std::vector<PersistentVoiceSet::Entry> &bucket : set.buckets) {
      unsigned w = 0;

      for (unsigned r = 0; r < (unsigned)bucket.size(); r++)
        if (
          bucket[r].sampler->m_ResidenceToken.load(std::memory_order_relaxed)
          == bucket[r].token)
          bucket[w++] = bucket[r];
      if (w != bucket.size()) {
        bucket.resize(w);
        changed = true;
      }
    }
    set.staleSeen.store(0, std::memory_order_relaxed);
  }

  /* Arrivals since the previous period (Add): bucket each voice by its
   * precomputed (binIndex, channels) key - set at voice init by
   * ApplySubblockBin; reading it live used to pointer-chase
   * stream -> audio_section per voice per period - and stamp its unique
   * residence token. */
  set.inbox.Move();
  for (GOSoundSampler *sampler = set.inbox.ExchangeGetList(); sampler;) {
    GOSoundSampler *const next = sampler->next;
    const unsigned key = sampler->stream.GetBucketKey();
    const uint64_t token
      = s_ResidenceTokenCounter.fetch_add(1, std::memory_order_relaxed) + 1;

    if (key >= set.buckets.size())
      set.buckets.resize(key + 1);
    sampler->m_ResidenceToken.store(token, std::memory_order_relaxed);
    set.buckets[key].push_back({sampler, token});
    changed = true;
    sampler = next;
  }

  if (changed || set.prefix.size() != set.buckets.size() + 1) {
    unsigned offset = 0;

    set.prefix.resize(set.buckets.size() + 1);
    for (size_t k = 0; k < set.buckets.size(); k++) {
      set.prefix[k] = offset;
      offset += (unsigned)set.buckets[k].size();
    }
    set.prefix.back() = offset;
    set.total.store(offset, std::memory_order_relaxed);
  }
}

unsigned GOSoundGroupTask::GetGroup() { return AUDIOGROUP; }

unsigned GOSoundGroupTask::GetCost() {
  return m_ActiveSet.total.load(std::memory_order_relaxed)
    + m_ReleaseSet.total.load(std::memory_order_relaxed)
    + m_ActiveSet.inbox.GetCount() + m_ReleaseSet.inbox.GetCount();
}

bool GOSoundGroupTask::GetRepeat() { return true; }

bool GOSoundGroupTask::HasClaimableWork() const {
  const unsigned done = m_Done.load();

  if (done == 0)
    return true; // fresh period - a helper can become its drainer
  if (done != 1 && done != 2)
    return false; // draining (its publish will broadcast) or complete
  const uint32_t gen = m_Generation.load(std::memory_order_relaxed);
  const uint64_t ca = m_ActiveCursor.load(std::memory_order_relaxed);
  const uint64_t cr = m_ReleaseCursor.load(std::memory_order_relaxed);

  return ((uint32_t)(ca >> 32) == gen
          && (uint32_t)ca < m_ActiveSet.total.load(std::memory_order_relaxed))
    || ((uint32_t)(cr >> 32) == gen
        && (uint32_t)cr
          < m_ReleaseSet.total.load(std::memory_order_relaxed));
}

void GOSoundGroupTask::Run(GOSoundThread *pThread) {
  if (m_Done.load() == 3) // has already processed in this period
    return;
  {
    GOMutexLocker locker(
      m_Mutex, false, "GOSoundGroupTask::Run.beforeProcess", pThread);

    if (!locker.IsLocked())
      return;

    const unsigned done = m_Done.load();

    if (done == 4)
      /* Another thread is draining, deliberately without holding m_Mutex
       * (see below). Don't queue up behind it: the task repeats, so the
       * scheduler or Finish's help loop routes us back here once the
       * drain is published. */
      return;
    if (done == 0) // the first thread entered to Run()
    {
      /* Claim the drainer role (state 4), then run the drain WITHOUT the
       * mutex. Only the drainer touches the epoch arrays and the sort
       * scratch: state 4 excludes a second drainer, Reset()/WaitAndClear()
       * wait the state out, and stragglers of the previous period only
       * ever read the OLD buffer pair. The drain used to run under
       * m_Mutex; its per-period hold (a full pointer-chasing pass over
       * every sampler) made all participants - the TIME_CRITICAL audio
       * thread included - pile up behind a mutex without priority
       * inheritance (2026-07-10 profile: 14% of worker time and 50% of
       * the audio thread's time were blocked on this acquisition).
       *
       * Drain the linked lists into flat arrays once (single-threaded by
       * state 4). Subsequent threads claim batches via fetch_add on the
       * indices instead of competing with one compare_exchange per voice.
       *
       * Epoch flip: rebuild the *other* buffer pair so a straggler worker
       * still reading the previous period's arrays never has them mutated
       * under it, and carry over anything it never claimed (see the
       * members' comment in the header). */
      m_Done.store(4);
      locker.Unlock();

#if GO_SCHED_DIAG
      const auto drainT0 = std::chrono::steady_clock::now();
#endif
      /* Persistent-bucket period window (see the header). Close both
       * cursors first: the generation bump makes every straggler's
       * gen-checked CAS fail, so no new claim can start. */
      const uint32_t newGen
        = m_Generation.load(std::memory_order_relaxed) + 1;
      const uint64_t closed = ((uint64_t)newGen << 32) | 0xFFFFFFFFull;
      const uint64_t prevActive
        = m_ActiveCursor.exchange(closed, std::memory_order_acq_rel);
      const uint64_t prevRelease
        = m_ReleaseCursor.exchange(closed, std::memory_order_acq_rel);

      /* Wait out every straggler of the previous generation: claimers are
       * registered participants, so ActiveCount==0 guarantees nobody is
       * mid-copy over the vectors (they may reallocate below) and nobody
       * is still RENDERING a voice the new generation will re-claim (the
       * double-render hazard). Normally 0 already - a nonzero count here
       * is the same pathological late-period case the old design handled
       * with its epoch double-buffer, bounded by one voice batch. */
      while (m_ActiveCount.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();

      // Diagnostic for the "silent voices never end" investigation:
      // claimable ranges left over means the previous period ended without
      // processing everything. Nothing to preserve - the entries are
      // persistent and the new generation re-claims from position 0 - but
      // each occurrence still indicates a violated completion protocol.
      {
        const unsigned totalA
          = m_ActiveSet.total.load(std::memory_order_relaxed);
        const unsigned totalR
          = m_ReleaseSet.total.load(std::memory_order_relaxed);
        const unsigned activeLeftover = (uint32_t)(prevActive >> 32) + 1
            == newGen
          ? totalA - std::min((unsigned)(uint32_t)prevActive, totalA)
          : 0;
        const unsigned releaseLeftover = (uint32_t)(prevRelease >> 32) + 1
            == newGen
          ? totalR - std::min((unsigned)(uint32_t)prevRelease, totalR)
          : 0;

        if (activeLeftover || releaseLeftover)
          wxLogWarning(
            "GOSoundGroupTask::Run: %u active + %u release sampler(s) were "
            "never claimed/processed in the previous period - they stay "
            "resident and are re-claimed this period",
            activeLeftover,
            releaseLeftover);
      }

      ApplyPeriodWindow(m_ActiveSet);
      ApplyPeriodWindow(m_ReleaseSet);

      /* Open the new generation. Release order: a claimer whose CAS
       * succeeds on one of these cursors is guaranteed to see the window's
       * vector/prefix writes. */
      m_ActiveCursor.store((uint64_t)newGen << 32, std::memory_order_release);
      m_ReleaseCursor.store(
        (uint64_t)newGen << 32, std::memory_order_release);
      m_Generation.store(newGen, std::memory_order_release);

#if GO_SCHED_DIAG
      s_DrainUsSum.fetch_add(
        (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - drainT0)
          .count(),
        std::memory_order_relaxed);
#endif

      // List-size samples for the monitor (first thread, once per period).
      for (const size_t sz :
           {(size_t)m_ActiveSet.total.load(std::memory_order_relaxed),
            (size_t)m_ReleaseSet.total.load(std::memory_order_relaxed)})
        if (sz > 0) {
          uint64_t prevMax = s_ListSizeMax.load(std::memory_order_relaxed);

          s_ListSamples.fetch_add(1, std::memory_order_relaxed);
          s_ListVoicesSum.fetch_add(sz, std::memory_order_relaxed);
          while (prevMax < sz
                 && !s_ListSizeMax.compare_exchange_weak(
                   prevMax, sz, std::memory_order_relaxed))
            ;
        }

      /* Publish under the mutex: every condition-wait site (Finish,
       * Reset, WaitAndClear) checks its predicate under m_Mutex, so
       * signaling under it too rules out lost wakeups. Unconditional
       * lock, NO pThread bail-out: returning without publishing would
       * leave m_Done stuck at 4 and every later participant would back
       * off forever. */
      GOMutexLocker publishLocker(
        m_Mutex, false, "GOSoundGroupTask::Run.publish");

      /* Register BEFORE m_Done leaves state 4: once m_Done is 1, another
       * helper could register, process every batch and deregister; if the
       * drainer's own registration came after that, the helper's "last
       * thread" check would fire m_Done=3 while the drainer still has its
       * processing (and buffer merge) ahead of it. */
      m_ActiveCount.fetch_add(1);
      m_Done.store(1); // there are some threads in Run()
      // Wake anyone parked during the drain: Finish's help loop re-enters
      // Run and joins in; Reset/WaitAndClear re-check their predicates.
      m_Condition.Broadcast();
    } else {
      if (!HasClaimableWork())
        return;
      m_ActiveCount.fetch_add(1);
    }
  }

  // Captured after the mutex block: the generation only changes during a
  // period window (state 4), and threads only get here after registering
  // in states 0->publish or 1/2, so it is stable for this participation -
  // and it is what makes this participant's claims fail once the NEXT
  // period's window has closed the cursors.
  const uint32_t myGen = m_Generation.load(std::memory_order_relaxed);

  // several threads may process the same list in parallel helping each other
  // at first, they fill their's own buffer instances
  GO_DECLARE_LOCAL_SOUND_BUFFER(localBuffer, 2, GetNFrames())

  /* The local accumulator is silenced lazily, by ProcessList, right before
   * the first claimed batch renders into it - and the merge below is
   * skipped entirely when this participant claimed nothing. A participant
   * without loot used to memset 1 KB of silence and ADD it onto the SHARED
   * task buffer under the mutex: semantically a no-op, physically pure
   * DRAM + cross-core cacheline traffic, multiplied by every helper
   * wakeup (measured 2026-07-10 as several GB/10s of extra writes). */
  bool hasLoot = false;

  unsigned processed = ProcessList(
    m_ActiveSet, m_ActiveCursor, myGen, false, localBuffer.GetData(), hasLoot);

  processed += ProcessList(
    m_ReleaseSet, m_ReleaseCursor, myGen, true, localBuffer.GetData(), hasLoot);
  // pThread == nullptr: this participant IS the audio thread (Finish help
  // or the Exec broom) - feed the monitor's "CB work" share curve.
#if GO_SCHED_DIAG
  if (pThread == nullptr && processed > 0)
    s_CbVoices.fetch_add(processed, std::memory_order_relaxed);
#endif

  {
    GOMutexLocker locker(
      m_Mutex, false, "GOSoundGroupTask::Run.afterProcess", pThread);

    if (locker.IsLocked()) {
      if (m_Done.load() == 1) {
        // The first thread is finished. Assign the result to the common
        // buffer - ALWAYS, even without loot: someone must overwrite the
        // previous period's content, or an empty (or entirely
        // foreign-processed) period would output stale audio.
        if (!hasLoot)
          localBuffer.FillWithSilence();
        CopyFrom(localBuffer);
        m_Done.store(2); // some thread has already finished
      } else if (hasLoot) {
        // not the first thread. Add the result to the common buffer
        // (skipped without loot - adding silence is a no-op that only
        // generates shared-buffer cacheline traffic)
        AddFrom(localBuffer);
      }
    }
    // Underflow-guarded decrement: with balanced inc/dec (see Reset()) this
    // never sees 0, but if the balance is ever violated again, a plain
    // fetch_sub would wrap to UINT_MAX and permanently break the
    // last-thread detection for all future periods.
    unsigned active = m_ActiveCount.load(std::memory_order_relaxed);

    while (active > 0
           && !m_ActiveCount.compare_exchange_weak(
             active, active - 1, std::memory_order_acq_rel))
      ;
    if (active <= 1) {
      /* Straggler guard: only complete a period that is actually in a
       * processing state (1/2). A worker from the PREVIOUS period that
       * deregisters while the new period is still idle (0) or draining
       * (4) must not flip the fresh period to "all done" - that would
       * skip it entirely (its voices only survive via the leftover
       * carryover) and wake Finish with incomplete output. This hazard
       * predates the lock-free drain; the drain's mutex-free window just
       * widened it enough to demand the guard. */
      const unsigned d = m_Done.load();

      if (d == 1 || d == 2) {
        // the last thread
        m_Done.store(3); // all threads have finished processing this period
        m_Condition.Broadcast();
      }
    }
  }
}

void GOSoundGroupTask::Exec() {
#if GO_SCHED_DIAG
  if (m_Done.load() != 3)
    // The period is still unfinished at Exec time: the RT thread is about
    // to run the leftovers synchronously (the "Exec broom").
    s_ExecLatePeriods.fetch_add(1, std::memory_order_relaxed);
#endif
  m_Stop.store(true);
  Run();
  /* If a drain was in flight, Run() above backed off without processing.
   * Exec must not silently skip the period (its samplers would only
   * survive as leftovers), so help-wait for the publish. Bounded: the
   * drain is a fraction of a millisecond, and this path only runs at
   * period boundaries where a concurrent drain is already pathological. */
  while (m_Done.load() == 4) {
    std::this_thread::yield();
    Run();
  }
}

void GOSoundGroupTask::Finish(bool stop, GOSoundThread *pThread) {
  if (stop)
    m_Stop.store(true);
  /* Help-then-wait loop. Run() participates when it can; it returns
   * without participating while another thread drains (state 4). In that
   * case we park on the condition and get woken by the drain's publish
   * broadcast - then LOOP BACK and help processing, instead of the old
   * single wait that would have idled this thread (possibly the audio
   * thread) for the whole period. The second wake source is the last
   * worker's m_Done=3 broadcast, which terminates the loop. */
  for (;;) {
    // pThread == nullptr identifies the TIME_CRITICAL audio thread - its
    // participation is policy-gated (helping at full load evicts a worker
    // mid-claim; see GOSoundOrganEngine::MayCallbackHelp).
    if (pThread != nullptr || m_engine.MayCallbackHelp()) {
#if GO_SCHED_DIAG
      if (pThread == nullptr)
        s_CbHelpRuns.fetch_add(1, std::memory_order_relaxed);
#endif
      Run(pThread);
    } else {
#if GO_SCHED_DIAG
      s_CbHelpDenied.fetch_add(1, std::memory_order_relaxed);
#endif
    }
    if (m_Done.load() == 3)
      return;

    GOMutexLocker locker(m_Mutex, false, "GOSoundGroupTask::Finish", pThread);

    if (
      !locker.IsLocked() || m_Done.load() == 3
      || (pThread != nullptr && pThread->ShouldStop()))
      return;
    m_Condition.WaitOrStop("GOSoundGroupTask::Finish", pThread);
  }
}

void GOSoundGroupTask::WaitAndClear() {
  GOMutexLocker locker(m_Mutex, false, "ClearAndWait::WaitAndClear");

  // wait for no threads are inside Run() - including a mutex-free drainer
  // (state 4), which must never see Clear() pull the arrays out from
  // under it
  while (m_Done.load() != 0 && m_Done.load() != 3)
    m_Condition.WaitOrStop("ClearAndWait::ClearAndWait", NULL);

  // Now it is safe to clear because m_Mutex is locked and no other threads can
  // enter in Run()
  Clear();
}
