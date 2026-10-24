/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDGROUPTASK_H
#define GOSOUNDGROUPTASK_H

/* Scheduler-anatomy diagnostics (drain timing, callback-help counters,
 * "CB work" share). All counters are relaxed atomics and one steady_clock
 * pair per period - practically free - but per project rule everything
 * measuring inside the audio path must be quickly removable: set to 0 to
 * compile the instrumentation out (the monitor then shows zeros). */
#define GO_SCHED_DIAG 1

#include <atomic>
#include <vector>

#include "sound/playing/GOSoundSamplerList.h"
#include "sound/scheduler/GOSoundTask.h"
#include "sound/scheduler/GOSoundThread.h"
#include "threading/GOCondition.h"
#include "threading/GOMutex.h"

#include "GOSoundBufferTaskBase.h"

class GOSoundOrganEngine;

class GOSoundGroupTask : public GOSoundBufferTaskBase {
private:
  GOSoundOrganEngine &m_engine;
  GOMutex m_Mutex;
  GOCondition m_Condition;

  // the number of threads are processing the samples
  std::atomic_uint m_ActiveCount;

  // processing state
  //   0 - no threads are processing the samples
  //   1 - some threads are processing the samples and none has finished
  //   2 - some thread has finished processing samples but not all
  //   3 - all threads have finished processing samples
  //   4 - the period's first thread is draining the lists into the epoch
  //       arrays WITHOUT holding m_Mutex (see Run: the drain used to run
  //       under the mutex and its ~0.5 ms hold made every participant -
  //       the TIME_CRITICAL audio thread included - queue up behind a
  //       non-priority-inheriting mutex; measured 2026-07-10 as 14% of
  //       worker time and 50% of the audio thread's time). While in this
  //       state only the drainer may touch the epoch arrays/sort scratch;
  //       everyone else backs off (Run returns, Reset/WaitAndClear wait).
  std::atomic_uint m_Done;
  std::atomic_bool m_Stop;

  // Voices are claimed from the shared queue in batches of this size so that
  // prefetch hints within a batch are guaranteed to target this thread's own
  // next voice rather than one already taken by a competing thread.
  static constexpr unsigned VOICE_BATCH_SIZE = 32;

  // ---- Persistent voice buckets (2026-07-11 redesign) ----
  //
  // Replaces the per-period drain + bucket sort of every voice (measured
  // ~60ns per voice per period, SERIAL on the period's first thread: ~35%
  // of the whole 128-sample budget at 14k voices, ~22% at 256). Voices are
  // bucketed by their (binIndex, channels) key ONCE at arrival and stay
  // resident; the per-period window costs O(changes), not O(polyphony).
  //
  // Membership: an Entry captures the sampler pointer plus its residence
  // token (GOSoundSampler::m_ResidenceToken) at insertion. A voice leaves
  // by TOKEN INVALIDATION only - death (ReturnSampler) and migration to
  // the release processor store 0, and the pool's memset covers recycling
  // - so its entry goes stale and is skipped by the claim copy (lazy
  // removal) until a later period window compacts it away. Tokens are
  // globally unique and never reused, so a recycled sampler re-inserted
  // anywhere can never resurrect an old entry (no ABA).
  //
  // Claiming: one 64-bit cursor per set - generation (period counter) in
  // the high 32 bits, position within the virtual concatenation of all
  // buckets (prefix[] offsets) in the low 32. Claims are CAS loops, NOT
  // fetch_add: a straggler of an old generation must FAIL (a fetch_add
  // would irrevocably consume a new-generation range and lose those
  // voices for the period). Unclaimed ranges of a finished generation
  // need no carry-over - the entries are persistent, the next generation
  // re-claims everything from position 0.
  //
  // Mutation happens ONLY in the period window (first thread, m_Done
  // state 4), which first closes both cursors (generation bump => every
  // straggler CAS fails) and then waits until m_ActiveCount drops to 0:
  // every claimer is a registered participant, so afterwards nobody can
  // be mid-copy over the vectors (they may reallocate) and no straggler
  // can still be RENDERING a voice the new generation would re-claim -
  // the double-render hazard the old design solved with its epoch
  // double-buffer.
  struct PersistentVoiceSet {
    struct Entry {
      GOSoundSampler *sampler;
      uint64_t token;
    };
    // Lock-free MPSC arrival queue (Add); emptied by the period window.
    GOSoundSamplerList inbox;
    std::vector<std::vector<Entry>> buckets; // by GOSoundStream bucket key
    std::vector<unsigned> prefix;            // buckets.size()+1 offsets
    std::atomic<unsigned> total{0};          // == prefix.back()
    // Stale entries encountered by claim copies since the last compaction
    // (each stale entry is re-skipped every period until compacted).
    std::atomic<unsigned> staleSeen{0};
  };
  PersistentVoiceSet m_ActiveSet;
  PersistentVoiceSet m_ReleaseSet;
  std::atomic<uint64_t> m_ActiveCursor{0};
  std::atomic<uint64_t> m_ReleaseCursor{0};
  std::atomic<uint32_t> m_Generation{0};

  // ---- Per-voice decoded render blocks (2026-07-08 redesign) ----
  //
  // Superseded the shared/atomic/pooled decoded-subblock cache and its
  // organ-wide two-phase collect/merge/barrier machinery entirely (see
  // tools/compression/INVESTIGATION.md): a real pipe never sounds twice
  // at once, so no two simultaneously active voices ever need to
  // coordinate over the same decoded window - every GOSoundStream owns
  // its own small decoded render block outright (see GOSoundStream.h).
  // Consequently there is nothing here to sort, deduplicate, or claim
  // across workers: the SAME worker that already claimed a batch of
  // voices for rendering (the existing VOICE_BATCH_SIZE claim below)
  // simply decodes-then-renders that batch itself, in two local passes,
  // with zero cross-worker coordination. Voices needing a rebuild are
  // grouped by channel count into groups of up to
  // GOSoundCompressionCacheV3Batch::LANES for the AVX2 batch kernel; a
  // lone voice (or the non-AVX2 fallback) uses the scalar path.

  // Processes one claimed batch of voices: classifies every voice
  // (GOSoundStream::ClassifyForRender), fuses BatchRender voices into
  // 8-lane groups that decode+resample+fade+accumulate on one warm
  // scratch (Phase 2, GOSoundResampleBatch), builds BuildOnly voices'
  // blocks via GOSoundStream::BuildBlocksBatch and renders them (and
  // everything else) through the classic per-voice path. Purely local
  // to this worker - no locking, no cross-worker state. Replaces the
  // former separate decode pass + render loop.
  void ProcessVoices(
    GOSoundSampler *const bufs[],
    unsigned count,
    GOSoundSampler *const nextBufs[],
    unsigned nextCount,
    bool toDropOld,
    float *output_buffer,
    unsigned prefetchAhead);

  // Applies one set's period window - compaction (once enough stale
  // entries accumulated) and inbox insertion - and rebuilds prefix[] and
  // total when membership changed. Only the period's first thread (m_Done
  // state 4) may call this, after closing both cursors and waiting out
  // m_ActiveCount (see the PersistentVoiceSet comment).
  void ApplyPeriodWindow(PersistentVoiceSet &set);

  // Claims batches of `set`'s virtual concatenation via the gen-checked
  // CAS cursor and processes their live entries into output_buffer.
  // myGen: the generation this participant registered for - claims of any
  // other generation fail harmlessly. hasLoot (in/out across the
  // active+release call pair): set when the first LIVE voice is claimed -
  // which is also when output_buffer gets zeroed (lazily; a participant
  // without loot never touches the buffer, see Run's merge logic).
  // Returns the number of voices this participant processed.
  unsigned ProcessList(
    PersistentVoiceSet &set,
    std::atomic<uint64_t> &cursor,
    uint32_t myGen,
    bool toDropOld,
    float *output_buffer,
    bool &hasLoot);

public:
  // Cumulative V3 per-voice decode counters (see the atomics in the .cpp),
  // engine-wide across all group tasks. Read by the Audio Monitor, which
  // derives per-interval rates and includes them in the audio report.
  struct DecodeStats {
    uint64_t rebuilds;      // voices whose block/scratch was decoded
    uint64_t batchCalls;    // multi-lane kernel calls (build + fused render)
    uint64_t soloRebuilds;  // single-voice (scalar) rebuilds
    uint64_t batchRendered; // voices through the fused decode+render path
    uint64_t syncFallbacks; // synchronous mid-render rebuilds (GOSoundStream)
    uint64_t listSamples;   // non-empty flat lists observed (per period+task)
    uint64_t listVoicesSum; // total voices across those lists
    uint64_t listSizeMax;   // longest list since the previous GetDecodeStats
    uint64_t cbVoices;      // voices processed BY the audio thread itself
    uint64_t cbHelpRuns;    // audio-thread Finish participations
    uint64_t cbHelpDenied;  // participations denied by the help policy
    uint64_t drainUsSum;    // µs spent in the lock-free drain (all tasks)
    uint64_t execLate;      // Exec calls that found the period unfinished
    // In-situ stage cycles of the fused path (GO_STAGE_PROF; zeros when
    // compiled out): decode incl. fused transpose, per-lane FIR,
    // fader+accumulate, admin remainder of the group flush.
    uint64_t stDecodeCy;
    uint64_t stFirCy;
    uint64_t stFaderCy;
    uint64_t stFusedCy;
    uint64_t stAdminCy;
  };
  static DecodeStats GetDecodeStats();

  /* Cheap, lock-free hints for helper loops (GOSoundReleaseTask): whether
   * this period is complete, and whether a helper entering Run() right
   * now would find something to do (claimable batches, or a fresh period
   * needing its drainer). Racy by design - a stale answer only delays or
   * wastes one help attempt; the Run()/Finish() protocol under m_Mutex
   * stays the sole source of truth. */
  bool IsPeriodDone() const { return m_Done.load() == 3; }
  bool HasClaimableWork() const;

  GOSoundGroupTask(
    GOSoundOrganEngine &sound_engine, unsigned samples_per_buffer);

  unsigned GetGroup();
  unsigned GetCost();
  bool GetRepeat();
  void Run(GOSoundThread *pThread = nullptr);
  void Exec();
  void Finish(bool stop, GOSoundThread *pThread = nullptr);

  void Reset();
  void Clear();
  void Add(GOSoundSampler *sampler);
  void WaitAndClear();
};

#endif
