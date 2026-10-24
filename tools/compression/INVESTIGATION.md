# Compressed-sample decode: investigation summary & handoff

Written 2026-07-07 by Claude (Anthropic), for whoever continues this work
(human or AI). Goal was **decode speed** of GO's compressed 24-bit sample
cache (`GOSoundCompressionCache`), not RAM/compression ratio — user's
explicit priority throughout. All numbers below are measured, not
estimated; every tool referenced is in this directory and reproducible.

**Update, same day (2026-07-07), second half of the session:** Step 1's
core codec is no longer just benchmarked, it is now implemented as real
production code — see "Step 1 implementation status" below before reading
the (now slightly historical) plan section that follows it.

**Update, same day (2026-07-07), later Codex handoff:** Phase 2.5 has been
extended with segment-aware render subblocks and loop-jump handling. This
captures the later finding that GO stores prepared loop EndSegments
separately instead of writing crossfades into one global sample buffer.

**MILESTONE: three-way production profile comparison (2026-07-10, user's
Windows machine, same organ, same 13600-voice load, ETW sampling
profiles with matched -O3/-g builds).** Compared: (a) a reference build
of commit 9b42c1c79 - the state immediately BEFORE any V3 production
code, with all earlier optimizations (planar SIMD polyphase etc.), built
from a worktree with RELEASE_FLAG=ON and the same profile flags (note:
the build-on-linux.sh wrapper has an ordering bug - it sets
WX_CONFIG=$MINGW_DIR/bin/wx-config BEFORE sourcing set-mingw-vars.sh, so
MINGW_DIR must be passed in the environment; and an empty release flag
silently produces a -Og DEBUG build - both bit us once); (b) today's
build with V3 disabled (legacy compression); (c) today's build with V3
enabled. Main render-path self time: baseline **295.5 s** / legacy today
**297 s** / V3 **~210 s**; decode share ~106 / ~110 / ~57-62 s; FIR ~189
/ ~189 / ~149 s (fused batch renderer beats the old AVX2 FIR by ~21%).
Conclusions: (1) the V3/GroupTask rework cost the legacy path NOTHING
(295.5 vs 297 s - no regression); (2) V3 delivers ~29% less main-path
work end-to-end in production, decode nearly halved, proven against two
independent references; (3) the GroupTask Finish barrier (~18-23%
inclusive) exists identically in ALL THREE profiles - a long-standing
structural property (wait for the slowest batch at period end), NOT a
rework regression; with V3's smaller compute it becomes relatively more
important (Amdahl) and is now the top remaining perf target
(candidate: smaller claim batches toward the end of the voice list);
(4) GetCurrentPtr (the scheduler's cross-voice prefetch hint) costs
0.7-1.2% self in every variant (worst under V3, whose live-position
computation is heavier) while the prefetch feature historically bought
only 1-2% - re-evaluate with a PrefetchDistance=0 A/B before optimizing
the hint. The pre-V3 reference binaries incl. PDBs live in Code
Exchange under "GrandOrgue Dev/bin-pre-v3/".

**Feature (2026-07-10, user idea, implemented, cache version 3):
uncompressed prefix - voice STARTS never decode.** Motivation: the user
observed callback-time spikes on chord releases under the subblock mode.
Diagnosis: a voice start pays (a) the skip-decode from sample 0 to the
aligned start position P (state build-up; this cost also existed under
legacy - the release-alignment LUT only ever delivered a POSITION, never
a decoder state, so the first seek decoded 0..P) plus (b) the build of
the voice's first render block, both inside the audio callback (InitStream
and InitAlignedStream build no block; the first build happens in the
GroupTask decode pass). Fix: each V3 section additionally stores its
first N samples as raw planar PCM (native bit depth) next to the
compressed data - pure redundancy, ~0.1% of the compressed size.
Dimensioning (GOSoundProviderWave): attacks get a constant 512+8 samples
(one max render block + FIR guard - "all keys pressed at once" costs
zero decode); releases get `2*T + 512 + 8` where T is the pipe period
from the recording MIDI note (fallback 2048), because the aligned start
position scatters over [0,T) and the correlation search space reaches
2T. Mechanics: BuildBlockScalar serves any block starting inside the
prefix directly from raw PCM (no decoder state, `m_HasBlockEndState =
false`); the prefix end position is added as a regular seed snapshot, so
the first post-prefix block finds its state via the existing
`FindV3SnapshotAtOrBefore` - no special cases downstream;
BuildBlocksBatch redirects prefix-range lanes to the scalar path. The
raw store keeps `prefix + getMaxReadAhead()` samples so a clipped block
at the seam takes its FIR guard from raw data too. Serialized in the
.cache file as version 3 (version bump invalidates existing V3 caches
once). RAM cost estimate for the 52 GB organ: ~50-150 MB. Expected
effect: chord-release (and mass-attack) spikes drop to near-uncompressed
cost; verify by repeating the callback-time-graph spike test.
**Follow-up fix (same day, after the user reported only marginal
improvement):** the first drop only wired the prefix into
BuildBlockScalar/BuildBlocksBatch - the BuildOnly path. But a freshly
started voice classifies as BatchRender (>99.8% of voices) and goes
through DecodeGroupToScratch -> PrepareBlockState, which has no
dependent-forward state yet and skip-decoded scalar from the
section-start seed to the aligned position: the entire voice-start cost
was still alive on the main path. ClassifyForRender now returns
BuildOnly while a block starts inside the prefix (raw-copy build +
classic render, decode-free); the voice joins the fused path at the
prefix-end seed. Lesson: when adding a fast path, enumerate EVERY
consumer of the slow path it replaces - the fused render path bypasses
the VoiceBlock build entirely and needed its own routing rule.

**First real-world full-organ data point (2026-07-08, user's Windows
machine, largest available sample set, V3 enabled, read from the organ
settings statistics):** whole-organ average compression ratio **1.78x**
(some ranks >2x), total sample memory 52 GB with V3 vs 62 GB with legacy
— i.e. ~92.6 GB loaded raw (incl. GO-generated end segments), legacy
back-computes to ~1.49x (right at its known 1.5x ceiling). This is
meaningfully ABOVE the 4000-file fleet benchmark's 1.53x average,
plausibly because a full organ's bulk is long, quiet, smooth release
tails where V3 hits 1-byte blocks (3x block ceiling) while legacy can
never go below 2 bytes/sample. V3's RAM saving on this set: **10 GB
(16%) vs legacy** — a side benefit; speed was the design goal.

**Design decision (2026-07-08, user architecture): two-phase callback
with owner-assigned batch decode - NOT speculative async preparation.**
Supersedes Phase 2.5's "workers should ensure the current render
subblock, and usually the next one, are built before the renderer needs
them" wherever that reads as speculative/async work: in a real-time
callback there is no "later" - the decode work per callback is a fixed
quantity (samples x pitch x voices), so building ahead asynchronously
only moves work around, risks the block NOT being ready when needed
(renderer then waits = contention), and adds synchronization cost for
no smoothing benefit. Instead the callback splits into two phases:
Phase A (decode): deterministically compute every subblock this callback
needs PLUS ONE SUBBLOCK OF LOOKAHEAD (user clarification 2026-07-08: the
audio buffer is never aligned to subblock boundaries, so a callback
always straddles an "old" and a "new" subblock - building, in the
current callback's decode phase, the block whose boundary falls in the
NEXT callback costs nothing extra in steady state (it is the same one
new block per subblock period, just built one callback earlier) and
buys a full callback period of jitter tolerance: the next callback then
only ever reads Ready views. This lookahead is a fixed part of the
Phase A span - same thread pool, same ownership, same barrier -
deliberately NOT "some other thread builds it eventually"). Per voice:
exact source span from position + pitch increment + FIR guard,
`ceil((frames-1)*inc) + 8`, extended to the next subblock boundary;
include the loop jump target when end_pos falls inside the span.
DEDUPLICATE (unison/couplers: many voices, one pipe, one build),
distribute as 8-lane batches across workers with clear per-batch
ownership - zero collisions, no cache mutex, one phase barrier.
Phase B (render): all voices read from Ready views, lock-free. No
dedicated orchestrator thread needed: GO's existing GroupTask claim
pattern (fetch_add on a flat array) IS distributed orchestration and
Phase A reuses it over subblock jobs; the phase barrier matches GO's
existing task-completion synchronization (m_ActiveCount/condition).
(Slice 2's synchronous-mutex miss path stays as the safety fallback and
instrumentation baseline.)

**Implementation status (2026-07-08, slice 3 landed):** the design above
is now production code. `GOSoundDecodedSubblockCache` stores one atomic
`Entry*` slot per render subblock (lock-free Ready reads, CAS-claimed
ownership, release-store publish; the map+mutex of slice 1/2 is gone),
Phase A builds deduplicated RUNS of consecutive subblocks
(`BuildRuns`: scalar chains, or the 8-lane AVX2 wavefront via
`GOSoundCompressionCacheV3Batch` - now runtime-dispatched with
`target("avx2")` so the production binary keeps its baseline ISA),
`GOSoundStream::CollectNeededSubblocks` reproduces ReadBlock's segment
walk exactly (same integer resampling arithmetic, loop-jump target
included, one-subblock lookahead), and `GOSoundGroupTask` runs the
two-phase period: first thread drains + plans (sort/unique/coalesce/pack
under the existing mutex), everyone claims decode batches via fetch_add,
one spin-then-condition phase barrier, then the unchanged Phase B render.
Tests: `GOTestSoundStream::TestSubblockBuildRuns` (bit-exact batch/scalar
builds incl. guard, short final subblocks, Ready-gap jumps) and
`TestTwoPhaseSpanPrediction` (300 looped iterations x 3 pitches with
pre-built predictions and the assertion syncBuilds == 0 - the prediction
covers every read, loop wraps included).

**Hard lesson from the first Windows load test (2026-07-08, user
report - the build froze the entire OS under load):** the read path's
wait-for-another-builder was an unbounded yield-spin. GO's audio threads
run at (near-)realtime priority; once every core hosted a spinning
realtime waiter, the preempted builder thread never ran again and the
spinners starved all of Windows. Any wait on the audio hotpath MUST
escalate to a real `sleep_for` (release the core), never yield-spin
indefinitely - `waitUntilReady` now spins ~512 iterations, yields ~512
more, then sleeps 100 us per retry. Keep this rule for every future wait
in this subsystem (the phase barrier already blocks properly on the
task's condition variable).

**Design decision (2026-07-08, user correction): NO persistent
render-subblock snapshot grid.** A first implementation of plan step 10's
"snapshot grid metadata" (a resumable codec snapshot every 512 samples,
serialized with the cache - briefly committed as cache version 2, then
reverted) was rejected on a RAM argument the user raised and won: at 40
bytes per 512 samples it costs ~1.2 GB of RAM + cache size on a ~90 GB
organ, for a capability playback never actually needs. Every real jump
target IS a StartSegment (section start, every loop start - snapshots
already exist there since the first V3 commit); everything else is
sequential: a running voice's next render subblock inherits its start
state from the end of the previous one (the plan's dependent-forward
rule), and post-alignment release starts land within one pipe period of
the release's own StartSegment. A blanket grid answers "random entry
into the middle of a section with no running context" - a question the
audio path never asks. The decoded render-subblock cache (step 10) must
therefore source its build states from StartSegment snapshots and live
decoder state only. (The V3 cache-file roundtrip test written alongside
the grid was kept - it is grid-independent and covers the previously
broken save/load path.)

**Parked idea (2026-07-08, user suggestion): keep the uncompressed PCM
around until release alignment is done.** Today `Setup()` compresses
immediately and discards the raw PCM; the release-alignment scans that
run later during loading (`ComputeTable`/`ComputeCorrelationLut`) then
decompress the whole section again - O(n) per scan since the SampleReader
fix, but still redundant decode work, several times per section. Keeping
the raw PCM until alignment completes (or computing alignment before
compressing) would eliminate all of it, at the cost of a transient load-
time RAM peak (alignment needs a pipe's attack AND release sections at
once). Load-time-only optimization, orthogonal to the audio hotpath.

**Parked idea (2026-07-07): FLAC/Zstd-style storage codec for the
on-disk `.cache` file — deliberately deferred, not started.** This is
orthogonal to everything else in this document: it would only affect
disk size and one-time cache-load time (I/O-bound, sequential, happens
once per organ load), never the audio-thread hotpath. Model: keep the
`.cache` file heavily compressed with a general-purpose or FLAC-like
codec; on load, decompress it once, sequentially, straight into V3's
runtime format in RAM. The audio thread would never see the storage
codec at all — no conflict with V3, no re-litigating any decision above.
Reasons to defer: (1) it solves a different bottleneck (I/O/disk size)
than this whole investigation's target (audio-thread decode speed); (2)
there is already a large amount of in-flight, uncommitted V3
integration work (see git status) that should land and be verified in a
real running GO first; two large parallel initiatives raises the risk
that neither finishes cleanly, the same reasoning behind deferring Step
2/3 above. Pick this up only once V3 is fully wired, toggled, and
confirmed working in a real organ — and only if cache load time or disk
size turns out to be an actual user-visible pain point, not just a
theoretical one.

## TL;DR

A new fixed-block, byte-granularity codec + AVX2 SIMD multi-voice decode
is **~4.75x faster** than GO's current per-sample variable-length decoder
on a 4000-file real-world sample, with **better** compression ratio
(1.53x vs 1.43x) in aggregate — though ratio varies by material (busy
multi-harmonic stops can lose to production's finer per-sample escaping;
see `codec_fleet_benchmark.cpp` header for the concrete numbers). A
further, narrower ~1.8x is available for genuine multi-mic-perspective
("surround") sample sets, but only when a rank has exactly 4 stereo
perspectives (8 channels) — fewer wastes SIMD lanes and the win disappears
(see "Step 3" below).

## The codec family (three building blocks, in order of maturity)

1. **v3** — `real_sample_compression_check.cpp` establishes the format
   ceiling of GO's *current* codec (exactly 1.500x on 24-bit material,
   unless residuals exceed the 14-bit escape threshold — see
   `codec_fleet_benchmark.cpp`'s "busy attack file" note). `block_width_check.py`
   → `planar_blockwidth_check.cpp` develop the replacement: **each voice
   compressed independently** (as real GO must — no future-batch
   knowledge at ODF-load time), fixed 1024-sample blocks, one shared
   byte-width per block (1/2/3/4 bytes) chosen from that voice's own
   residuals only. Decode batches 8 independent voices into one AVX2
   register via **planar bulk extraction per lane + 8x8 transpose**
   (`simd_multivoice_check.cpp`, `width_bucketing_check.cpp` "Approach C").
   Width=3 (24-bit, the only non-power-of-2 stride) uses a PSHUFB-based
   unpack (`codec_fleet_benchmark.cpp`), ~2x faster than the naive
   byte-OR scalar path. **This is what "v3" means in the implementation
   plan** — fully verified on 4000 real files, see `codec_fleet_benchmark.cpp`'s
   header comment for exact numbers (~4.75x vs production, 1.53x ratio).

2. **Variante C** (decode-time width bucketing) — `width_bucketing_check.cpp`
   tests whether pre-sorting a runtime batch by each voice's *already-known*
   current-block width (no format change, no future prediction — the
   width byte is read from the header before batching) beats v3's
   arbitrary-order batching. Verified across 5 implementation attempts
   (A=v3 baseline, B=naive per-sample gather across scattered buffers,
   C=planar bulk-extract+transpose per bucket, D=byte-repack into one
   buffer, E=AVX2 hardware gather `_mm256_i64gather_epi32`) — **only C
   wins, and only by ~0-10%**, reproducibly, on real broad samples (2
   independent 368-voice runs). AVX2 gather (E) is *slower* than v3 on
   this hardware — a real, measured result, not a guess. **Verdict: small
   win, real scheduler complexity (re-bucketing every ~1024 samples) —
   worth it only if profiling after Step 1 confirms it matters.**

3. **Surround/multi-perspective format** — `surround_preinterleave_check.cpp`
   tests the case where N mic perspectives of the *same pipe* are
   **always** decoded together (fixed group, perfectly time-aligned — no
   uncertainty about future batch membership, unlike arbitrary voices).
   Verified on **real** Arnstadt organ data (4 real perspectives:
   `01-dir`/`03-SennAB`/`04-MK2HAB`/`05-Rear` of `HWPrincipal8`, found via
   `find /media -iname surround...`-style search — see conversation for
   the exact paths). Key measured facts:
   - Forcing one **shared** width across all 4 perspectives (instead of
     each perspective's own independent width) costs **~0.07% ratio**
     (1.4989x vs 1.4999x) — because it's literally the same acoustic
     event, different mics, so per-block dynamics correlate almost
     perfectly. This is what makes the ORIGINAL (fastest, no-transpose)
     interleaved kernel legitimate here, unlike for arbitrary voices.
   - Width still changes per block (verified: 374/375 blocks width=2, 1
     block width=3, handled correctly, roundtrip OK) — nothing here
     assumes a globally-fixed width, just a per-block *shared* one.
   - Naive decode-time byte-interleaving of *independent*-width
     perspectives (`decodePre` in `surround_preinterleave_check.cpp`) is
     **slower**, not faster, than v3 (0.9151 vs 0.2288 ns/channel in one
     run) — because it still branches per lane per sample inside the hot
     loop. The speed only comes from the *shared width* (branch-free
     kernel), not from pre-interleaving alone.
   - **Crossover math** (see conversation, last exchange): AVX2 always
     processes all 8 lanes in fixed time, so unused lanes (padded with a
     duplicate of a real lane) aren't free per real channel — effective
     cost per real channel = `nominal_per_lane × 8 / N_real`. Measured
     nominal ~0.331 ns/channel (shared) vs ~0.383 ns/channel (v3) →
     **crossover at N≈6.9 of 8 lanes**. Only worth it at **exactly 4
     stereo perspectives (8 channels, no waste)**; 3 or fewer perspectives
     → v3 is better or equal. User's correction (2026-07-07): don't
     under-rate this — large/demanding sample sets that push machines to
     their limits are disproportionately likely to be exactly the
     4-perspective surround sets, so the audience isn't as narrow as the
     "only 8-lane-exact" framing initially suggested.

## Step 1 implementation status (2026-07-07, second half of the session)

The codec core described as "Step 1" below is now **real production code**,
not a benchmark harness:
`src/grandorgue/sound/playing/GOSoundCompressionCacheV3.h`. It deliberately
mirrors `GOSoundCompressionCache`'s member names and call shape
(`Init`/`DecompressionStep`/`DecompressTo`/`m_position`/`m_value`/`m_prev`/
`m_last`/`m_ptr`) so it can slot into the same call sites with minimal
churn. It is **not yet wired into `GOSoundAudioSection` or
`GOSoundStream`** — see "Remaining wiring work" below for exactly what's
left and why it was deliberately not attempted in the same sitting.

**What changed from the original plan, and why (read this before trusting
anything above uncritically):**

1. **The format is one continuous encode of the whole section, not
   per-segment re-encodes.** The original plan's mental model ("blocks are
   self-contained if we store block-start... predictor reset") was
   subtly wrong. Reading `GOSoundAudioSection::Compress()` (the real
   encoder) showed that GO does not re-encode audio per loop segment at
   all — `m_StartSegments` are just alternate resumable *entry points*
   into one single continuous compressed stream covering the whole
   section (`for i in 0..m_SampleCount`), captured via a state snapshot
   (`m_StartSegments[j].cache = state`) taken mid-stream. A first attempt
   at v3 that forced a fresh block boundary at every segment start (to
   dodge the "how do you snapshot mid-block" problem) produced silent
   data corruption the moment a segment's *final* block was short: the
   decoder has no way to know a block held fewer than 1024 samples before
   hitting what it thinks is the next block's width byte, so it
   misinterprets that byte and everything downstream. This was caught by
   `v3_production_roundtrip_check.cpp`'s segment-boundary test, not by
   reasoning about the format — exactly the kind of bug the "no test
   harness ever tried non-sequential access" gap (below) was warning
   about. Fixed by supporting genuine **mid-block snapshots**: a block's
   byte width is only known once the whole block has been scanned, so
   `Encode()` takes a sorted list of snapshot positions up front, buffers
   the pre-sample predictor state for each one as it scans, and resolves
   the exact byte offset retroactively once each block's width is fixed.
   See the `Snapshot` struct and `Encode()`'s doc comment in the header.

2. **Single-voice speedup is modest (~1.05-1.85x on real files), not
   4.75x.** The 4.75x number in the TL;DR above is real, but it is the
   combined effect of the fixed-width format *and* batching 8 independent
   voices into one AVX2 register (planar extraction + transpose). That
   batching is a separate, later integration (rewiring
   `GOSoundGroupTask`'s claim order + a new shared decoded render subblock
   cache — still not started). Scalar, single-voice V3 decode next to
   scalar single-voice legacy decode — the fair comparison for what Step
   1 alone buys before any batching work — was re-measured against the
   real header on the same 33-file real-sample spread used elsewhere in
   this doc: 1.05-1.85x, format-dependent (bigger win exactly on the
   "busy attack" files where legacy's variable-length code is forced into
   its 3-4 byte escape path most often, e.g. `073-C#.wav` /
   `057-A.wav`/`062-D.wav` in the Arnstadt/Rotterdam set above: 1.7-1.85x).
   Do not repeat the 4.75x figure as if it were what single-voice
   integration alone delivers — it isn't measured yet whether real
   production batches of 8 arbitrary concurrent voices actually reach the
   full number end-to-end (this was already flagged as an open risk
   below; still open).
   **First correctness pitfall, also caught by testing, not reasoning:**
   the very first working version of `readResidual`/`writeResidual`
   called `std::memcpy(&v, p, width)` with `width` a *runtime* value.
   Every one of the harness prototypes switched on a compile-time-constant
   width instead; this version didn't, and it cost ~7x — scalar V3 decode
   measured at ~10.8 ns/frame vs legacy's ~1.5 ns/frame, the *opposite* of
   the goal, because a runtime-length `memcpy` compiles to a real function
   call per sample instead of one inlined mov. Fixed by switching on
   `width` inside `readResidual`/`writeResidual` so each case is a
   compile-time-constant-size access the compiler can inline. **Lesson:
   never let a byte-width parameter reach memcpy as a runtime value in
   this codec family again** — verify with a disassembly or a speed
   regression check if the decode loop is ever refactored.

**What is verified, against the real header, on 33 real files spread
across the Alessandria/Nancy/Rotterdam/Arnstadt/Caen/Friesach/etc.
collections (`v3_production_roundtrip_check.cpp`):**
- Sequential whole-file decode is bit-exact vs source PCM.
- Resuming decode from a `Snapshot` taken at an arbitrary, deliberately
  NOT block-aligned position (simulating a loop/attack start) is
  bit-exact — the actual open risk flagged in the original TL;DR/plan.
- The backward-seek fallback (`DecompressTo` called with a position
  before the cache's current position, as happens on some release-align
  correlation lookups) resets and replays correctly.
- All 33/33 files passed every check. (One placeholder file,
  `BlankLoop.wav`, failed to load in the tool's minimal WAV parser -
  unrelated to the codec, not investigated further; it's a silence
  placeholder, not representative sample data.)

**Remaining wiring work — SUPERSEDED (2026-07-07, wiring completed later
the same day; kept for history, do not treat as open):** items 1-3 below
are now all implemented and committed:
1. DONE — `GOSoundAudioSection` has `CompressV3()` (continuous whole-
   section encode with sorted mid-block snapshot positions resolved into
   `m_StartSegmentsV3`, early-abort fallback to uncompressed when the
   encode doesn't pay off), and `SaveCache()`/`LoadCache()` read/write
   the full V3 payload (explicit magic/version/codec header +
   StartSegmentV3 snapshots). Legacy caches remain byte-stable.
2. DONE — `GOSoundStream::getDecodeBlockFunction` dispatches on the
   codec axis (`getCompressedDecodeBlockFunction<CacheT>`), and
   `GoToStartSegment`/`GetCurrentPtr`/`GetHistory` all handle V3
   (snapshot load via `LoadSnapshot(pData, channels)`, following the
   same disguised-byte-offset `m_ptr` convention as legacy).
3. DONE — opt-in setting threaded through
   `GOConfig`/`GOSettingsOptions`/`GOPipeConfigNode`/the wave+tremulant
   providers down to `Setup(..., useV3Compression, ...)`.
4. DONE (automated part) — `GOTestSoundStream` now covers V3 end-to-end
   through the REAL playback path: all format x interpolation x channels
   combinations of `TestReadBlock`, plus the 500-iteration loop-wrap
   capture test comparing V3 (and legacy) bit-exactly against the
   uncompressed reference — including segment transitions, loop wraps
   resuming from StartSegmentV3 snapshots, and skip-ahead Seek()s. Found
   and documented along the way: **V3 legitimately falls back to
   uncompressed on very short sections** (the block format always
   reserves full BLOCK_SAMPLES slots per channel, so a few-hundred-sample
   section with any residual wider than 1 byte exceeds its own raw PCM
   size — the early-abort fallback handles this correctly, but tests had
   to use a small-amplitude wave and assert the codec actually engaged,
   or the V3 legs would silently test nothing; real short samples, e.g.
   very short releases, will simply stay uncompressed). Still open: the
   manual listen-test with a real organ loaded and V3 enabled, and the
   8-voice AVX2 batching integration (reworking `GOSoundGroupTask` +
   the Phase 2.5/2.5b decoded render-subblock cache).

## Batched multi-voice decode kernel (same day, third part of the session)

Point 4 above turned out not to need waiting after all - the batched
8-voice AVX2 kernel is now also implemented as production code:
`src/grandorgue/sound/playing/GOSoundCompressionCacheV3Batch.h`
(`GOSoundCompressionCacheV3Batch::DecodeBatch`). This is what the 4.75x
figure in the TL;DR actually depends on - **the fixed-width format alone
(single-voice, no batching) only gains ~1.05-1.85x**; a user correction
mid-session ("v3 is 4.75x faster") surfaced that this distinction hadn't
been stated clearly enough the first time it was written up, so: it was
never a property of the format by itself, it has always required
batching multiple independent voices into one AVX2 register together
(planar extraction + 8x8 transpose). See `codec_fleet_benchmark.cpp`'s own
header comment for the original breakdown: `production 1.354  our scalar
1.278 (1.06x)  our SIMD 0.285 (4.75x vs production, 4.48x vs our own
scalar)` - i.e. even in the original numbers, scalar-alone was only
~1.06x; nearly all of the 4.75x always came from the SIMD batching.

**What's now verified, against the real production headers, on 24 real
files across 3 independent 8-file batches**
(`tools/compression/v3_batch_multivoice_check.cpp`):
- Bit-exact vs sequential single-voice `DecompressionStep` for both a
  full 8-lane batch and a **partial 5-lane batch** (the padding path -
  see the bug below).
- Lanes are deliberately **phase-misaligned**: each of the 8 voices in a
  batch starts decoding from a different, arbitrary, non-block-aligned
  position in its own stream, simulating real voices claimed into a
  batch mid-flight rather than all starting together - the original
  `codec_fleet_benchmark.cpp` prototype only ever tested lanes that
  started at position 0 in lockstep, so it never actually exercised this,
  even though real `GOSoundGroupTask` batches are exactly this case.
- Real, measured stereo speedup: **~1.5-1.9x on top of** the single-voice
  format's own ~1.05-1.85x (i.e. combined, meaningfully less than 4.75x
  total vs true legacy - the honest current number, not yet the target).
  Lower than the original mono-only benchmark's 4.75x for a real,
  understood reason (not a fluke): see bug/gap 2 below.

**One predictor-convention detour and one real bug found while
generalizing the original mono-lane, phase-aligned-only prototype into
this general form - do not undo the current code without re-running the
tools:**
1. **Predictor rounding convention.** An intermediate production port
   tried to match legacy `GOSoundCompressionCache` exactly by using `/2`
   (C++ truncation toward zero) in the predictor, which required a
   `truncHalf()` correction in the AVX2 path because
   `_mm256_srai_epi32(diff, 1)` computes floor division for negative odd
   diffs. The project later deliberately moved V3 back to the original
   benchmark's `>> 1` convention everywhere. This is not lossy: V3's
   encoder computes residuals with the same predictor its decoder uses,
   so decoded PCM remains bit-exact to the source samples. It only means
   V3's residual stream is not bit-identical to legacy's residual stream,
   which is acceptable for a new versioned cache format.
2. **A genuinely new bug in the promotion step, not inherited from the
   benchmark.** The first version of `DecodeBatch`'s padding for
   `numLanes < LANES` pointed every unused lane directly at
   `lanes[0].cache` (aliasing, not copying) and tried to fix the
   resulting corruption with a save-before/restore-after of lane 0's
   state around the whole call. That still corrupted lane 0, because the
   per-lane loops *inside* the call (block-header refresh, chunk-size
   computation, extraction, and the final pointer/position advance) all
   mutate `*padded[l]` in place, once per lane per chunk - with several
   padding slots aliased to the same object, that one object got advanced
   multiple times per chunk (e.g. 4x for a 5-of-8 batch), and no amount
   of restoring it *after* the call undoes wrong intermediate state that
   already fed into the shared AVX2 predictor registers during the call.
   Fixed by giving each padding slot an **independent scratch copy** of
   lane 0's cache instead of a second pointer to the same object - no
   restore needed at all, because nothing shared gets mutated. Caught by
   adding a `numLanes=5` case to `v3_batch_multivoice_check.cpp`; the
   original all-8-lanes-only test could never have caught it. **Lesson
   for whoever touches this next: any "pad the batch to a fixed width"
   trick anywhere in this codec family must use independent copies, never
   aliased pointers, if the batch kernel mutates lane state in place.**

**Update, same session, immediately after: closed the width==3 gap by
switching the block layout from interleaved to planar-per-channel.** The
paragraph above (kept in git history, not reproduced here) originally
said the width==3 PSHUFB fast path only works for contiguous residuals,
that real stereo content therefore falls back to a scalar loop for that
case, and that fixing it was "not yet attempted." The user pushed on this
directly ("will you change from interleaved LRLRLR... stereo to planar to
get 4.75x speedup?") and was right that it was the correct next lever, not
a marginal one.

**The format changed** (`GOSoundCompressionCacheV3`'s block layout is now
`[width:1][channel 0's BLOCK_SAMPLES residuals][channel 1's BLOCK_SAMPLES
residuals]...`, each channel's region always reserving the full
BLOCK_SAMPLES slots regardless of the block's true length, instead of
samples' channels alternating byte-by-byte). This is a bigger change than
it sounds: it is NOT just a batch-kernel tweak, it required reworking the
single-voice encoder AND decoder too (`Encode()`'s write loop,
`DecompressionStep()`'s addressing, and `Snapshot`/`LoadSnapshot`, which
now needs a `channels` parameter to locate the next block's header). Full
re-verification after the rewrite: **all 33 single-voice files still
bit-exact** (roundtrip, segment-resume, backward-seek - the earlier
"Step 1 implementation status" section's checks), and **all 24
batch-kernel files still bit-exact** (both `numLanes=8` and the
`numLanes=5` partial-batch case). Nothing was taken on faith - the exact
same test suites were rerun after the format change, not assumed to still
pass.

With planar-per-channel, `extractChannelPlanar` dropped the `stride`
parameter entirely (residuals are now contiguous for any channel count),
which let width==3 always use the PSHUFB trick (not just for mono), AND
made widths 1/2/4 into single SIMD sign-extending loads
(`_mm256_cvtepi8_epi32` / `_mm256_cvtepi16_epi32` / a plain 256-bit load)
instead of scalar per-sample loops - a further win the interleaved format
never allowed at all, for any width.

**Real, measured result** (same 3 independent 8-file real batches, same
phase-misalignment methodology as before): batching's contribution over
8x sequential single-voice V3 decode went from ~1.5-1.9x (interleaved) to
**~2.4-2.7x (planar)**. Single-voice-alone speedup vs legacy is
essentially unchanged (~1.0-1.9x, format layout doesn't matter for
scalar decode - the planar change is entirely a batching-kernel win).
Combined, this is meaningfully closer to the original mono-only
benchmark's 4.75x than the interleaved attempt was, though still not
identical to it (that number was mono; this is real stereo content, and
the final V3 predictor convention deliberately matches the original
benchmark's `>> 1` convention rather than legacy `/2`). **Do not restate
"4.75x" as achieved** - restate the actual current number, and re-measure
whenever anything in this file changes.

## Phase 2.5: decoded-cache layout and consumer-loop validation

Authoritative user specification:

Before implementing full SIMD rendering, validate the decoded-cache layout
against the actual consumers in GO's audio hotpath.

The SIMD batch decoder should not scatter its output into per-lane split
buffers inside the inner loop. Its primary output layout should be
lane-interleaved:

```text
decoded[channel][sample * LANES + lane]
```

This preserves the original AVX2 store advantage: each decoded source
sample produces one contiguous SIMD store for all active lanes.

However, this layout is only clearly optimal if the following
render/resample/mix code also consumes the same lanes as a batch. A scalar
per-sampler consumer sees a fixed stride:

```text
decoded[channel][sample * LANES + lane]
```

This access pattern is prefetchable, but not necessarily
bandwidth-efficient if the other lanes are not processed soon enough to
reuse the same cache lines.

Phase 2.5 therefore measures and decides the decoded-cache layout
strategy:

- Keep the SIMD batch output in lane-interleaved layout:
  `out[channel][sample * LANES + lane]`.
- Add a compatibility scalar read path that can read one lane from this
  layout.
- Benchmark three consumer variants:
  - scalar renderer reading from normal contiguous per-voice decoded
    blocks;
  - scalar renderer reading one lane from lane-interleaved batch blocks;
  - batch renderer consuming all lanes in sample-major order.
- Measure whether the old scalar renderer can tolerate the fixed lane
  stride with hardware prefetching, or whether it loses too much
  cache/bandwidth efficiency.
- If scalar consumption is too slow, do not use lane-interleaved batch
  cache as the general decoded cache. Instead keep two layouts:
  - scalar decoded cache: channel-/stereo-friendly contiguous layout for
    the existing renderer;
  - SIMD batch decoded cache: lane-interleaved layout for batch decode +
    batch render.
- Do not treat this as a cosmetic API choice. The output layout is part
  of the SIMD algorithm. Decoding into a SIMD register and then
  immediately scattering into split per-lane buffers destroys a major part
  of the measured speedup.
- Surround/perspective grouping must not assume that multiple stereo
  perspectives automatically share all runtime parameters. A true
  surround batch requires shared voice decisions:
  - same attack variant selection;
  - same random speaking/tuning factor;
  - same pitch increment;
  - same source position;
  - same loop/release transition state;
  - per-perspective gain/routing only.

Deliverable:

- A benchmark comparing split/scattered output, lane-interleaved output
  with scalar consumption, and lane-interleaved output with batch
  consumption.
- A documented decision whether GO needs one decoded cache layout or
  separate scalar and batch cache layouts.

Codex notes/current implementation state:

The final runtime shape is not "batch decode directly into a
callback-local per-lane output buffer." The batch decoder is supposed to
build an **uncompressed decoded render subblock cache** that worker
threads can share/use. Workers should ensure the current render subblock,
and usually the next one, are built before the renderer needs them.
Subblocks remain on the table so duplicate work from racing workers is
capped to roughly an audio-callback-sized region instead of a whole large
block.

`GOSoundCompressionCacheV3Batch` currently has two outputs:
- `DecodeBatch(...)`: compatibility/baseline layout,
  `out[lane][channel][sample]`.
- `DecodeBatchInterleaved(...)`: SIMD/cache layout,
  `out[channel][sample * LANES + lane]`, preserving one contiguous AVX2
  store per decoded source sample.

`tools/compression/v3_batch_multivoice_check.cpp` now validates both
layouts bit-exactly and adds a Phase-2.5 consumer benchmark. It compares:
- scalar renderer reading normal contiguous per-voice decoded blocks
  (approximates the current compressed polyphase read-ahead cache, whose
  channel taps are stride-1);
- scalar renderer reading one lane from lane-interleaved batch blocks
  (`decoded[channel][sample * LANES + lane]`, fixed stride 8);
- batch renderer consuming all lanes in sample-major order from the
  lane-interleaved cache.

Latest measurements on the three Claude batch files (`/tmp/batch8*.txt`,
same real stereo files, phase-misaligned lanes) with the current tool:

| batch | split decode + scalar consume | interleaved decode + scalar consume | interleaved decode + batch consume |
| --- | ---: | ---: | ---: |
| batch8 | 2.838 ns/channel-sample | 2.800 | 0.917 |
| batch8b | 3.226 | 2.969 | 0.990 |
| batch8c | 3.314 | 3.366 | 1.191 |

The scalar compatibility path is not catastrophic: scalar FIR-style
consumption from lane-interleaved memory was only about **1.08-1.17x**
slower than contiguous per-voice memory in these runs, so hardware
prefetching handles the fixed stride reasonably. But that small loss is
enough to cancel most of the decode-side interleaved-write gain if the
old scalar renderer remains the consumer. The large win appears only when
the consumer is also batch-shaped.

**Current decision:** do not make lane-interleaved batch blocks the only
general decoded cache for the existing scalar renderer. Keep the
lane-interleaved layout as the primary SIMD batch cache/output, but plan
for either (a) a separate scalar decoded cache layout for old rendering,
or (b) a real batch renderer that consumes `sample * LANES + lane`
directly. Revisit this only with a production-renderer benchmark, not by
looking at decoder speed alone.

Surround/perspective grouping also must not assume that multiple stereo
perspectives automatically share all runtime parameters. A true surround
batch requires shared voice decisions: same attack variant selection,
same random speaking/tuning factor, same pitch increment, same source
position, same loop/release transition state. Per-perspective gain and
routing may differ.


## Phase 2.5b: segment-aware render subblocks and loop jumps

This section refines Phase 2.5 for GO's actual loop playback model.
It is mandatory before wiring a decoded sub-block cache into the audio
hotpath.

### GO loop model to preserve

GO does **not** write loop crossfades into one global sample buffer. This
would be impossible for WAV files with multiple or overlapping loop
markers. Instead, `GOSoundAudioSection` builds separate prepared
EndSegments during section setup:

- the main sample data remains the normal linear source region;
- each WAV loop contributes its own StartSegment at the loop start;
- each WAV loop contributes its own EndSegment near the loop end;
- the EndSegment contains the prepared loop-end region, optional
  crossfade, and read-ahead samples from that loop's own start;
- file/release ends use zero read-ahead instead of loop-start read-ahead.

Runtime playback is therefore segment based:

```text
main segment until transition_position
EndSegment from transition_position to end_pos
when source >= end_pos:
    jump to next_start_segment_index + overflow
```

The sustain loop crossfade is not a second runtime sampler. The sampler
only sees a segment transition and then a source-position jump.

### EndSegment choice timing

The current EndSegment is already known while the sampler is approaching
its current loop end. It provides:

```text
transition_position
end_pos
next_start_segment_index
```

At `end_pos`, GO jumps to `next_start_segment_index`. The next EndSegment
for the next loop pass can be selected at that jump, because it is only
needed for the future pass.

For the decoded-cache scheduler this means:

- the jump target subblock at
  `startOffset(next_start_segment_index) + overflow` should be prepared
  before reaching `end_pos`, ideally at least one audio callback ahead;
- the next EndSegment for the next pass may be chosen when the jump is
  performed;
- the chosen next EndSegment then defines the next future
  `transition_position` / `end_pos`.

### Render subblocks

Render subblocks are runtime decoded source windows. They are distinct
from persistent compressed codec blocks.

Terminology must be precise here:

- **Codec block**: the persistent compressed V3 storage unit
  (`GOSoundCompressionCacheV3::BLOCK_SAMPLES`, currently 1024 samples).
- **Render subblock**: the runtime decoded source window consumed by the
  renderer/resampler. It may be smaller than a codec block and must
  include any guard samples required by interpolation and segment
  continuation.
- **SubblockStartSnapshot**: the decoder-start state required to build one
  render subblock independently and reproducibly. This is not merely "the
  marker state" and not merely "the last sample"; it is the exact V3
  predictor state at the first source sample of the render subblock, plus
  the encoded-data address needed to continue decoding from there.

A V3 SubblockStartSnapshot must contain, for the specific segment and
source position it belongs to:

```text
segment identity / source identity
subblockStartSourcePosition
encoded block data offset or pointer basis
offset inside the compressed codec block, if subblockStart is not block-aligned
block width / current block layout state
blockRemaining at subblockStart
m_position     = subblockStartSourcePosition
m_value[ch]    = decoded sample immediately before subblockStart (P - 1)
m_prev[ch]     = decoded sample two samples before subblockStart (P - 2)
m_last[ch]     = V3's saved older history field, matching exactly what
                 GOSoundCompressionCacheV3::Snapshot stores
```

At WAV/section start the predictor history is zero:

```text
m_value[ch] = 0
m_prev[ch]  = 0
m_last[ch]  = 0
```

Do not collapse this to "the last sample before the subblock". Before
decoding sample P, V3's live state is:

```text
m_value = sample[P - 1]
m_prev  = sample[P - 2]
```

and `DecompressionStep()` then does:

```cpp
m_last = m_prev;
m_prev = m_value;
m_value = m_prev + ((m_prev - m_last) >> 1) + residual;
```

So independent subblock construction needs both previous decoded samples
per channel and must preserve the code's `m_value`/`m_prev`/`m_last`
convention exactly. If this terminology drifts, the resulting bug will be
a small but recursive predictor error that may only show up after a
jump/loop boundary.

A render subblock must cover the source span required by the next render
chunk, not merely the output buffer length:

```text
ceil((outputFrames - 1) * sourceIncrement) + POLYPHASE_POINTS
```

Current GO polyphase uses 8 look-ahead samples:

```text
POLYPHASE_POINTS = 8
```

Therefore render subblocks must provide safe guard samples:

- near a loop end: guard-after samples must come from the loop-start
  continuation represented by the current EndSegment;
- near file/release end: guard-after samples must be zero;
- no polyphase read may cross a subblock boundary unless the required
  guard area or a validated next subblock is available.

Suggested sampler-side state:

```text
currentSubblock*
nextSubblock*
optional jumpTargetSubblock*
lane
sourceBaseIndex
sourceOffsetInSubblock
currentStartSegmentIndex
currentEndSegmentIndex
transition_position
end_pos
next_start_segment_index
```

### Hotpath rule

At a loop boundary, normal operation must not allocate, construct an
EndSegment, or perform a large locked cache operation.

Allowed in the hotpath:

```text
switch segment state
attach an already prepared jump target subblock
choose the next EndSegment for the future loop pass
```

Avoid as the normal path:

```text
constructing EndSegments
large cache lookups under locks
heavy synchronous decoding
scattering SIMD output into split per-lane buffers
```

A bounded synchronous fallback may exist for safety, but it must not be
the expected path.

## Implementation plan (agreed 2026-07-07)

**Step 1 (v3, foundation):**
- New versioned cache format (old caches stay readable; runtime
  toggle/fallback to legacy for A/B testing — this touches the real-time
  sound engine, treat it with the caution that implies).
- Integrate as single-voice decode FIRST (no batching) to validate loop
  points, seeking/backward jumps, pitch-interpolation look-back — **none
  of this was covered by the standalone test harnesses**, which only ever
  decoded straight through from position 0. This is the actual remaining
  correctness risk, not the codec math itself.
- Then: new shared decoded render sub-block cache (not merely per-sampler
  decode state). Worker threads request/build the source window containing
  the current playback position, and usually the next one; misses can be
  grouped into 8-lane decode batches. The cache must be globally reachable
  by the workers that claimed those voices/batches. Decode-ahead
  granularity must be decoupled from audio buffer length (which can change
  at runtime) and from pitch ratio (source-frame consumption per callback
  != output buffer length when pitch != 1). Phase 2.5 decides whether this
  cache needs separate scalar and batch layouts before any full renderer
  wiring is attempted. Phase 2.5b then makes these subblocks segment-aware:
  MainSegment, EndSegment, loop-start jump targets, polyphase look-ahead,
  and loop-boundary prefetch must all match GO's current playback semantics.
- Real open risk flagged but not yet resolved: production's actual
  runtime batches (`GOSoundGroupTask`'s flat-array claim order) are
  essentially arbitrary voices across the whole organ, not same-rank
  groups — verify real registrations' width-heterogeneity before trusting
  the 4.75x number end-to-end in production, not just in the benchmark's
  stride-sampled-across-the-collection methodology (which is a reasonable
  proxy but not identical to real multi-stop chords).

**Step 2 (Variante C, optional):** build only after Step 1 is running and
profiled for real; ~10% gain must be weighed against real scheduler
complexity (bucketing every block). Not obviously worth it — decide with
real data, not the synthetic benchmark alone.

**Step 3 (surround/4-perspective format):** separate cache format, only
engaged when ODF-load-time detection finds a rank with exactly 4 stereo
perspectives (how to detect this from ODF metadata is itself still an
open question — not yet designed). Falls back to Step 1's format
otherwise (including padding-with-duplicate-lanes for 2-3 perspectives,
which the crossover math says isn't worth a dedicated shared-width path).

## Tools in this directory (chronological, roughly increasing sophistication)

- `real_sample_compression_check.cpp` — production's 1.500x ratio ceiling.
- `entropy_ratio_check.py` — Rice coding reaches ~2.3x (resolves "3-4x"
  folklore vs measured 1.5x — needs real entropy coding, which GO doesn't do).
- `block_width_check.py` — per-block adaptive width prototype (Python).
- `planar_blockwidth_check.cpp` — C++ version, established interleaved+
  fixed-width beats production on decode speed; M/S stereo transform
  tested and rejected (no benefit for GO's wide-mic'd recordings).
- `phase_align_check.py` — retracted idea (ratio-only benefit, not speed).
- `simd_multivoice_check.cpp` — the original multi-voice AVX2 batching
  finding (shared/joint width, no transpose needed) — fast but initially
  built on an unrealistic assumption (compress-time knowledge of future
  batch membership); later shown to be exactly right for **surround
  perspectives** instead (see Step 3 above).
- `codec_fleet_benchmark.cpp` — the "how do we really stand" tool, runs
  ratio+speed across a large, stride-sampled real file set. **Read its
  header comment for the current best numbers.**
- `width_bucketing_check.cpp` — the "Vorselektion" (pre-sorting) question,
  Approaches A-E, honestly measured incl. two real implementation bugs
  found and fixed along the way (state reset per block; output-storage
  contaminating the timing) — read the file header and this doc's
  "Variante C" section above before trusting any one number in isolation.
- `fresh_production_compare.cpp` — direct, same-run, same-files comparison
  against GO's real unmodified `GOSoundCompressionCache` (via `#include`
  of the actual header) — the rigorous cross-check tool whenever a "does
  this really beat production" question comes up.
- `surround_preinterleave_check.cpp` — the surround-perspective format
  test, validated on real Arnstadt organ multi-mic data.
- `v3_subblock_snapshot_check.cpp` — validates the SubblockStartSnapshot
  model before production cache wiring: grid snapshots at 64/128/256/512
  samples, exact `m_value`/`m_prev`/`m_last` boundary-state checks,
  independent subblock decode with 8 guard samples, direct jump targets
  into the middle of render subblocks, deterministic fallback from the
  previous dependent snapshot/state when an exact subblock-start snapshot
  is absent, and metadata/speed reporting. Runs on `/tmp/batch8.txt`,
  `/tmp/batch8b.txt`, and `/tmp/batch8c.txt`: 96/96 file/subblock-size
  checks passed, with 6 direct jump targets and 6 fallback targets per
  file/subblock-size.
- `v3_segment_subblock_check.cpp` — synthetic Phase-2.5b harness for
  segment-aware render subblocks: MainSegment vs prepared EndSegment,
  loop-start guard continuation, terminal zero guard, explicit
  `next_start_segment_index + overflow` jumps, jumpTargetSubblock
  preparation, same-virtual-position EndSegments with distinct SegmentIds,
  prepared `end_data` reads that intentionally differ from MainSegment PCM,
  and 8-tap FIR window equality. Current run: 9/9 segment/subblock checks
  passed.
- `v3_subblock_cache_check.cpp` — synthetic decoded render-subblock cache
  policy harness. It exercises blocking miss resolution, bounded
  duplicate-build allowance, try-lock sync fallback counting, immutable
  ready-entry checks, clean Building/Ready/Failed state separation, cache-key
  separation for SegmentId/layout/guard length, rejection/instrumentation of
  Ready overwrite attempts, no-large-build-under-map-lock accounting, and an
  8-worker race simulation. Current run: 5/5 cache policy/contract checks
  passed.
- `v3_subblock_builder_api_check.cpp` — prototype-level builder API check:
  scalar MainSegment V3 builder, batch lane-interleaved MainSegment V3
  builder, and uncompressed EndSegment copy/convert builder. Runs on
  `/tmp/batch8.txt`, `/tmp/batch8b.txt`, and `/tmp/batch8c.txt`: builder
  API checks passed.
- `v3_cache_format_contract_check.cpp` — prototype-level cache-format
  contract check for production V3 plumbing. It verifies legacy-prefix
  byte stability, new-reader recognition of legacy compressed/uncompressed
  prefixes, explicit V3 magic/version/codec fields, and rejection of future
  versions, invalid codec values, and truncated prefixes. It now also checks
  the current production-loader policy: new-header None/Legacy are accepted,
  while V3 is rejected until payload support exists. Current run: cache
  format contract checks passed.

## Known unknowns (do not assume these are solved)

- Segment-aware decoded render subblocks: GO-compatible loop-jump target
  preparation, polyphase guard samples, and cache behavior across loop
  boundaries are not wired or production-tested yet (the Phase 2.5/2.5b
  design + synthetic harnesses exist, the production cache does not).
- ~~Loop-point/seeking correctness through the full `GOSoundStream` /
  resampler / EndSegment playback path~~ — RESOLVED 2026-07-07:
  `GOTestSoundStream` now verifies V3 bit-exactly against the
  uncompressed reference through real `ReadBlock()` playback including
  segment transitions, loop wraps from StartSegmentV3 snapshots, and
  skip-ahead Seek()s (see "Remaining wiring work — SUPERSEDED" above).
  Still outstanding on top of that: a manual listen-test with a real
  organ and V3 enabled (synthetic PCM only so far), and pitch-shifted
  (non-1:1 resampling) coverage of the compressed paths.
- Real (not stride-sampled) multi-voice batch composition in GO's actual
  scheduler — does width heterogeneity in real chords match the
  benchmark's proxy?
- ODF-level detection of "these N files are perspectives of one pipe" —
  not designed.
- Cache memory footprint / eviction policy at realistic polyphony counts —
  not measured (order-of-magnitude estimate only, see conversation).
- V3 on very short sections: falls back to uncompressed by design (full
  BLOCK_SAMPLES slot reservation per block/channel) — correct behavior,
  but nobody has measured yet what fraction of a real organ's sections
  (short releases etc.) this affects in practice.

## Where to find the human collaborator's context

This session's memory files (auto-memory system, not part of the repo):
`project_stage_timing_results.md` (earlier, narrower decode-cost
breakdown that motivated this investigation), `feedback_test_over_speculation.md`
(the user's core working preference: every claim gets tested on real
data, not reasoned about in the abstract — this entire investigation
follows that pattern and any continuation should too),
`feedback_language.md` (communicate in German with this user),
`feedback_version_bump.md` and `feedback_precommit_only_for_pr.md` (repo
workflow conventions). A new memory entry summarizing this whole
compression investigation and pointing here should be added to
`MEMORY.md` if it doesn't already reference this file.

## Persistent buckets (design, 2026-07-11 - the measured 60ns/voice drain killer)

**Motivation (self-measured, Audio Monitor "Sched:" line):** the period's
first thread drains both linked lists into flat arrays and bucket-sorts
them EVERY period - ~60ns per voice per period, serial. At 128-sample
buffers that is ~920us of a 2667us budget (35%!), at 256 ~22%. It is the
single largest remaining structural cost, codec-independent, and exists
only because voices re-insert themselves into the linked lists every
period (ProcessSamplerPostRender -> Add) and the arrays are rebuilt from
scratch.

**Design (user's): period cost proportional to CHANGES, not polyphony.**
- Per group task, per (binIndex, channels) bucket key: a persistent
  vector<GOSoundSampler*>. A voice is APPENDED once at start
  (ApplySubblockBin knows the key - it is the cached m_BucketKey), and
  removed by swap-with-last when it dies or migrates (order within a
  bucket is irrelevant - that is exactly why the bucket sort was stable).
- ProcessSamplerPostRender's "keep" return no longer re-inserts; instead
  "don't keep" enqueues an explicit REMOVAL (lock-free MPSC list, applied
  by the period's first thread - O(deaths), not O(voices)). Task
  migration (release -> detached windchest) = removal here + insertion
  there, at note-event rate.
- Claiming: keep the dynamic fetch_add batches (they are jitter
  absorbers, measured), but over a per-task table of bucket ranges
  (prefix sums, rebuilt only when membership changed - note-event rate).
  One atomic cursor over the virtual concatenation, exactly like today's
  flat array.
- Straggler protection replaces the epoch double-buffer: claims carry a
  period GENERATION number; a straggler's stale claim (old generation)
  processes nothing. The removal application and prefix-sum rebuild
  happen in the (now tiny) first-thread window - still under the
  existing m_Done state machine, states unchanged.
- The leftover-carryover safety net stays: unclaimed ranges of a
  generation are simply still there next period (the arrays are
  persistent - nothing to carry).

**Expected:** 128er ceiling ~14k -> ~19k (Rotterdam), 256er ~19k -> ~23k;
the drain line in the monitor is the direct before/after metric.

**Risks:** same class as the epoch protocol (silent voices) - removal
races (a voice dying while a straggler still renders it), generation
wraparound, migration double-insert. Mitigation: removals only applied
in the first-thread window; a removed voice's slot swap must not move a
sampler INTO a range another thread is still claiming from - hence
removals apply only between generations, never mid-period.

## Sprint milestone (2026-07-12 00:20): full-stack matrix, drain eliminated

Stufe-3 binary (persistent buckets + transpose render D + fused-prefetch
fix), automated ceiling sweep, margin criterion (headroom = callback peak
< 95% of compute peak in > 10% of window polls). Rotterdam, i9-10900KF,
20 workers, ASIO.

| sustained clean polyphony | 128    | 256    | 512    |
|---------------------------|--------|--------|--------|
| V3                        | 17 578 | 20 040 | 21 843 |
| legacy                    | 17 600 | 19 723 | 21 338 |

- **Sched drain: 0 us/s at 17.5k voices** (was ~345 000 us/s at 15.7k) -
  the buckets redesign fully eliminated the serial per-period drain.
- Both codecs +23-25% at 128 vs the pre-buckets baseline (~14.0k/14.3k).
- V3 wins the 256/512 cells (first time under equal criteria); the 128
  tie shows codec-neutral per-callback costs binding there (~400 fixed
  instructions/voice/callback, T_fix ~110us, OS jitter share).
- cbHelp 4900/s: with buckets the TIME_CRITICAL thread participates in
  voice work for the first time (was 0/s under the old scheduler).
- Open leads: V3 prefers fewer workers (HT siblings share L1; variant D
  needs ~20 KB/worker at 128) - 10-worker V3 column pending; next
  identified lever is the per-voice fixed-cost bundle (render-class
  caching, group-batched pre/post-render).

## Design sketch (2026-07-12 02:30, user): canonical per-rung coefficient streams

Source-driven FIR (consume interleaved decoder rows directly, scatter into
lane-parallel output accumulators) + the observation that a voice's
fraction walk is a deterministic arithmetic progression. If the
COEFFICIENT ROW CHOICE followed a canonical per-rung pattern (laid out in
memory in frame order at bin creation, "matched to the integer-phase
drift"), the lane-parallel coefficient problem collapses to a BROADCAST -
row load + broadcast + FMA would be the whole kernel.

Key separation that might make it sound-safe: keep per-voice EXACT
positions for sample indexing (pitch untouched); canonicalize only the
coefficient row selection. Neighboring table rows differ smoothly, so a
bounded row error is a tiny filter gain/phase ripple, not a pitch error.

Blockers to quantify BEFORE any production code:
1. Row-error tolerance: difference spectrum (exact vs offset-by-N rows)
   -> how many rows of drift are inaudible (target < -80 dB?); this sets
   the allowed rung width x block length product.
2. Lane-fill cost: simulate Rotterdam/Nancy pitch distributions against
   narrower (canonical) rungs - today's 7.6/8 fill lives on coarse rungs.
3. Reference numbers from tonight: coef assembly per output frame
   (8 lanes): runtime transpose 11.0 ns, tap-major-table gathers 17.9
   (L1-small 12.6) - broadcast would be ~1-2 ns. Variant D total budget
   22.6 ns/frame. AVX2 gathers lose even from L1 (Comet Lake microcode);
   the tap-major layout idea becomes attractive on AVX-512 hardware.

Related, independently useful: interpolated mini coefficient table
(6-7 phase bits + lerp, 2-4 KB, L1-resident) - helps every kernel form,
lerp is stall-shadow-free ALU.

Sharpening (user, same night): target the NEAR-1.0 CLASS first (small
resample factors - the untransposed bulk of sounding voices; quantify via
the Cache Bins histogram). Within cents of 1.0 the integer advance is
exactly 1/frame in >95% of blocks (at most ONE correction event per
block -> scatter masks degenerate to identity), the per-lane coefficient
row crawls 2-8 rows/frame, and if the row-error experiment permits
FREEZING the row per block (filter-phase ripple, not pitch), the
coefficient transpose drops from per-frame to per-block (~0.2 ns/frame).
One fat near-1.0 bucket also avoids the narrow-rung lane-fill penalty;
true repitches keep using variant D. Gatekeeper experiment: difference
spectrum exact-vs-frozen coefficient row over real detune spreads.

### Gatekeeper result (2026-07-12, coef_row_tolerance_gate.cpp)

Real production table, organ-ish signal, sample positions kept EXACT
(pitch untouched), error vs exact per-frame phase in dBFS:

| detune | freeze row/block | 7-bit grid (nearest) |
|--------|------------------|----------------------|
| 0.2 ct | -69 dB           | -75 dB               |
| 1 ct   | -44 dB           | -75 dB               |
| 10 ct  | -36 dB           | -75 dB               |
| 100 ct | -36 dB           | -75 dB               |

- **Freeze-per-block is DEAD**: audible (>-60 dB) above ~0.3 ct. Phase
  drifts ~47 rows/frame at 10 ct = ~6000 rows/128-block. The canonical/
  broadcast idea in pure form fails on physics.
- **Small phase table WINS**: 7-bit (128 levels, 4 KB, L1-resident) is
  -75 dB flat with plain nearest, no lerp. This is the codec-neutral
  latency lever - the 256 KB table (= L2 size) shrinks to L1, removing
  one dependent load per voice per frame, for D and every future kernel.
- Lerp branch (C) had a test-code wrap bug (came out worse than nearest,
  which is impossible for a smooth table) - the -75 dB nearest figure is
  a lower bound; correct lerp only improves it. Fix + verify before build.

## Matrix update (2026-07-12 17:11, mini-table + F2, 19 workers)

| sustained clean polyphony | 128    | 256    | 512    |
|---------------------------|--------|--------|--------|
| V3                        | 18 486 | 21 462 | 22 872 |
| legacy                    | 17 089 | 18 965 | 20 502 |
| V3 lead                   | +8 %   | +13 %  | +12 %  |

- V3 +5-7% vs the 00:20 matrix (F2 fused planar decode + mini table +
  one worker fewer); legacy -3-4%.
- The legacy column isolates the worker effect (F2 is V3-only, mini
  table measured neutral): -4% ceiling for -5% compute capacity
  (19/20 workers) - the cleanest confirmation yet that legacy is
  compute-bound and converts every logical thread into yield, while
  V3 (latency-bound) GAINED despite one worker less.
- Mini table (2 KB, -121 dB): ceiling-neutral on the 10900KF (the
  microbench ALU cost vanished in the stall shadow, the L2 relief did
  not show); kept for smaller-cache CPUs and the 254 KB L2 give-back.
- V3 now leads all six cells on Rotterdam - the least V3-friendly
  organ. 36 hours earlier it trailed legacy by ~10%.

## 20 vs 19 workers (2026-07-12 18:48, control cell)

| V3 @ 128 | 19 workers | 20 workers |
|----------|------------|------------|
| ceiling  | 18 486     | 18 418     |

V3 is worker-count-INDIFFERENT (18.49k vs 18.42k = noise) - latency-bound,
as predicted: the 20th thread neither helps (DRAM queue already full at 10
cores) nor hurts much. Legacy @128 went 17 089 (19w) -> 17 625 (20w),
i.e. legacy DOES gain from the extra logical thread (compute-bound). This
cleanly separates the two: worker count is a legacy knob, not a V3 one.
Full 20-worker matrix: V3 18418/21670/23139, legacy 17625/19421/20540.

### Callback-help policy: measured irrelevant (user, 2026-07-12)

Setting the TIME_CRITICAL thread's help policy to "never" changes the
128-buffer ceiling by exactly nothing (user tested quietly). Combined
with the earlier pre-buckets finding (cbHelp was 0/s - the callback sat
on the output-task mutex), and the post-buckets state (cbHelp 4900/s
without measurable effect either way): the audio thread's participation
in voice work is performance-neutral on this machine. The policy knob
stays as a diagnostic, not a lever. Also rules out the suspicion that
callback-help was distorting the edge criterion's saturation detection.
