# V3 decoded render subblock implementation plan

Date: 2026-07-07

This plan continues `tools/compression/INVESTIGATION.md`. Its purpose is to
turn the codec-speed investigation into a safe implementation path for GO's
audio hotpath.

The core correction is: the useful runtime unit is not "decode one compressed
sample on demand" and not "batch-decode into temporary per-lane buffers". The
useful runtime unit is a decoded, segment-aware render subblock that can be
shared by worker threads and consumed by the renderer/resampler.

## Current code state

Already implemented:

- `GOSoundCompressionCacheV3`
  - fixed-width 1024-sample codec blocks;
  - planar-per-channel residual layout;
  - `Snapshot` / `LoadSnapshot`;
  - arbitrary snapshot positions during `Encode()`;
  - backward `DecompressTo()` fallback by replaying from the beginning.
- `GOSoundCompressionCacheV3Batch`
  - 8-lane AVX2 batch decode;
  - split compatibility output;
  - lane-interleaved output:

```text
decoded[channel][sample * LANES + lane]
```

- `tools/compression/v3_batch_multivoice_check.cpp`
  - bit-exact checks for split and interleaved batch decode;
  - Phase-2.5 consumer-layout benchmark.
- `tools/compression/v3_subblock_snapshot_check.cpp`
  - verifies V3 SubblockStartSnapshots on a uniform render-subblock grid;
  - checks boundary predictor state exactly (`m_value`/`m_prev`/`m_last`);
  - checks independent subblock decode with 8 guard samples.

Not implemented:

- V3 format selection in `GOSoundAudioSection`;
- V3 cache serialization;
- V3 use in `GOSoundStream`;
- segment-aware decoded render subblock cache;
- production renderer consuming decoded subblocks;
- any production-safe scheduling around loop jumps.

## Risk guardrails from the planning discussion

Keep these risks visible while executing the plan:

1. Scheduling/cache-miss behavior in real audio playback may differ from
   standalone tools.
2. Locking/allocation can accidentally creep back into the audio hotpath.
3. The scalar renderer can lose the SIMD decode win if it reads the wrong
   decoded layout.
4. Cache serialization can break legacy cache compatibility.
5. Subblock guard samples and loop-overflow edges are easy to get subtly
   wrong.
6. Batch build can look good in tools while producing less benefit in real
   GO workloads.
7. Jump targets can silently corrupt audio if a render subblock is built with
   an assumed decoder state instead of the exact V3 state at its start.

## Non-negotiable terms

Use these names exactly in code comments, docs, and tests.

### Codec block

Persistent compressed V3 storage unit:

```text
GOSoundCompressionCacheV3::BLOCK_SAMPLES == 1024
```

Codec blocks are an on-disk/cache-file storage detail.

### Render subblock

Runtime decoded source window consumed by the renderer/resampler. A render
subblock may be smaller than a codec block and must include interpolation guard
samples or have a validated next subblock available.

Render subblocks are the unit the workers request/build/cache.

### SubblockStartSnapshot

The exact V3 decoder-start state needed to build one render subblock
independently and reproducibly.

This is not:

- a loop marker;
- merely a compressed byte offset;
- merely the last decoded sample;
- a renderer position;
- a per-callback temporary.

It is a codec state at the first source sample of a render subblock.

A V3 `SubblockStartSnapshot` must contain or wrap exactly the information
needed to restore `GOSoundCompressionCacheV3` before decoding the first sample
of the subblock:

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
m_last[ch]     = V3's saved older history field, matching Snapshot exactly
```

At section start:

```text
m_value[ch] = 0
m_prev[ch]  = 0
m_last[ch]  = 0
```

Do not simplify this to "store the previous sample". V3 predicts with both
previous decoded samples:

```cpp
m_last = m_prev;
m_prev = m_value;
m_value = m_prev + ((m_prev - m_last) >> 1) + residual;
```

A one-sample state mistake at a subblock boundary becomes recursive predictor
drift. Those bugs are small, persistent, and hard to locate later.

### Jump-target snapshot rule

For every direct jump to a `MainSegment` source position, the decoder state for
the render subblock containing that target must be available.

Short rule:

- Jump to position P -> state at the corresponding render-subblock start must
  be available.
- If no exact `SubblockStartSnapshot` exists for that render-subblock start,
  the builder must deterministically build from the previous dependent
  codec/render subblock state and decode forward from there.
- The builder must never guess, reset, or assume zero predictor state except at
  the real section start.
- Section start is the only valid V3 zero-state:

```text
m_position  = 0
m_value[ch] = 0
m_prev[ch]  = 0
m_last[ch]  = 0
```

For V3, the following fields must match the real encoder/decoder state exactly
before building a render subblock:

```text
m_position
m_value[ch]
m_prev[ch]
m_last[ch]
encoded block offset / pointer basis
offset inside the compressed codec block
block width
blockRemaining
```

During `CompressV3()`, capture the StartSegment, jump-target, and render
subblock grid snapshots while the encoder already has the correct state. Do
not try to reconstruct these states later from marker positions alone.

### Segment source

A decoded render subblock must identify which logical source it belongs to.

Initial segment kinds:

```text
MainSegment
EndSegment
```

`MainSegment` reads from the section's main sample data (`m_data`), compressed
or uncompressed depending on the section format.

`EndSegment` reads from `EndSegment::end_data`. In current GO, end segments are
never compressed:

```cpp
// GOSoundStream::InitFromSection
// End segments are never compressed
end_decode_call = getDecodeBlockFunctionFor(pSection, false, interpolationType);
```

Do not assume EndSegment samples can be rebuilt by decoding the main compressed
stream. They contain prepared loop-end material, optional crossfade, and
read-ahead from the loop start or zero fill.

## Target architecture

Runtime steady state:

```text
worker claims sampler(s)
worker determines source span needed by current render chunk
worker requests current render subblock
worker requests next render subblock
near loop end: worker requests jumpTargetSubblock
cache miss: worker builds subblock from SubblockStartSnapshot or EndSegment data
renderer/resampler consumes decoded subblock(s)
```

Normal loop-boundary hotpath must not:

- allocate;
- construct EndSegments;
- perform large locked lookups;
- synchronously decode a large missing block;
- scatter SIMD output into per-lane buffers inside the decoder inner loop.

A bounded synchronous fallback may exist for correctness, but it must be
instrumented and treated as a miss in performance tests.

## Chosen implementation order

The next meaningful work is not full SIMD rendering. It is to make decoded
render subblocks real, testable, and segment-aware.

Order:

1. Clean up stale docs/terms.
2. Add subblock snapshot/index support for V3 main data.
3. Build a standalone segment/subblock correctness harness.
4. Build a standalone subblock cache prototype.
5. Integrate V3 as an opt-in single-voice stream path using the existing
   renderer.
6. Replace direct compressed decode with decoded render subblock reads.
7. Add batch subblock build.
8. Add batch renderer.

Do not skip from step 2 to step 8.

## Step 0: documentation cleanup

Goal: remove contradictory wording before implementation starts.

Required edits:

- In `INVESTIGATION.md`, replace the remaining historical phrase
  "per-sampler decoded-block cache" with the current term
  "shared decoded render subblock cache".
- Ensure every use of "block" is qualified if ambiguity matters:
  `codec block`, `render subblock`, or `batch block`.
- Keep the Phase 2.5 authoritative user specification intact.
- Keep Phase 2.5b as the segment semantics source of truth.

Acceptance:

- `grep -n "per-sampler decoded-block" tools/compression/INVESTIGATION.md`
  returns no matches.
- `git diff --check` passes.

## Step 1: define production-neutral data types

Do this first in a tool/prototype header or a clearly isolated production
header. Avoid wiring into the audio hotpath until tests exist.

Suggested names:

```cpp
enum class GODecodedSegmentKind : uint8_t {
  MainSegment,
  EndSegment,
};

struct GODecodedSegmentId {
  const GOSoundAudioSection *section;
  GODecodedSegmentKind kind;
  unsigned segmentIndex; // 0 for MainSegment, EndSegment index for EndSegment
};

struct GOSoundV3SubblockStartSnapshot {
  GODecodedSegmentId segment;
  unsigned subblockStartSourcePosition;
  GOSoundCompressionCacheV3::Snapshot codec;
};

struct GODecodedSubblockKey {
  GODecodedSegmentId segment;
  unsigned subblockStartSourcePosition;
  unsigned sourceLengthIncludingGuard;
  unsigned channels;
  enum Layout {
    ScalarPlanar,
    BatchLaneInterleaved,
  } layout;
};
```

Important details:

- `segmentIndex` for `MainSegment` should be fixed at 0 unless later code
  proves a start-segment-specific identity is needed.
- `EndSegment` keys must include the actual EndSegment index because different
  loops can have different prepared `end_data`.
- `subblockStartSourcePosition` uses GO's virtual source coordinates for that
  segment, matching how `end_ptr` is currently made virtual:

```cpp
end_ptr = end_data - bytesPerSample * transition_offset;
```

- Do not key only by raw pointer. Pointers change across cache loads.
- Do not key only by absolute source position. EndSegments with the same
  virtual position are not necessarily the same audio.

Acceptance:

- Types are documented with the exact source coordinate convention.
- There is no hotpath allocation or locking in these type definitions.
- No renderer behavior changes yet.

## Step 2: choose provisional render subblock sizes

We need a buildable default for prototypes, but not a premature permanent
choice.

Prototype constants:

```text
candidateSubblockSamples = 64, 128, 256, 512
defaultPrototypeSubblockSamples = 128
```

Why 128 as prototype default:

- small enough to cap duplicate work if two workers race;
- matches earlier CHUNK=128 measurements;
- large enough to amortize metadata and cache lookup overhead in tests.

Do not hard-code this as final production policy.

Measurements required before final choice:

- snapshot memory overhead per minute of stereo 24-bit sample data;
- subblock build ns/channel-sample;
- renderer read ns/channel-sample;
- duplicate-build cost under simulated worker races;
- cache hit rate with realistic callback sizes and pitch increments.

Acceptance:

- Harness can run all candidate sizes.
- Results print subblock size in every output line.

## Step 3: generate SubblockStartSnapshots for V3 MainSegment

Current `GOSoundCompressionCacheV3::Encode()` already accepts sorted arbitrary
snapshot positions. Use that capability; do not invent a second predictor
capture path unless the existing one proves insufficient.

For each compressed main section:

```text
snapshotPositions = 0, subblockSize, 2*subblockSize, ...
also include any exact positions needed by legacy StartSegments if the
single-voice V3 stream path still uses StartSegment snapshots directly
```

Then:

```cpp
std::vector<GOSoundCompressionCacheV3::Snapshot> mainSubblockSnapshots;
Encode(..., snapshotPositions.data(), snapshotPositions.size(), ..., snapshots);
```

Index rule:

```text
snapshotIndex = subblockStartSourcePosition / subblockSize
```

only if all stored snapshots are on a uniform grid and position 0 is included.
If extra non-grid snapshots are mixed in, use an explicit table:

```text
struct SnapshotEntry {
  unsigned sourcePosition;
  GOSoundCompressionCacheV3::Snapshot snapshot;
};
```

Recommended initial approach:

- keep the uniform subblock grid separate from legacy StartSegment snapshots;
- do not mix grid snapshots and marker snapshots in one vector until the
  serialization format is designed.

Important:

- A render request starting at arbitrary source position `P` should usually
  use the subblock whose start is:

```text
subblockStart = floor(P / subblockSize) * subblockSize
offsetInSubblock = P - subblockStart
```

- The builder decodes the whole render subblock from `subblockStart`.
- The renderer consumes from `offsetInSubblock`.

Acceptance tests:

- For every candidate subblock size, building every subblock and concatenating
  the non-guard part reconstructs the original PCM exactly.
- Random `P` inside a subblock returns the same sample sequence as scalar
  `DecompressTo(P)`.
- Positions 0, 1, 2, subblockSize - 1, subblockSize, and final partial
  subblock are explicitly tested.

## Step 4: build `v3_subblock_snapshot_check.cpp`

Add a tool:

```text
tools/compression/v3_subblock_snapshot_check.cpp
```

Status 2026-07-07: implemented.

Purpose:

- verify that `SubblockStartSnapshot` can build independent decoded
  subblocks from real WAV files;
- verify exact state semantics at subblock boundaries;
- verify jump targets into the middle of render subblocks use the exact state
  at the containing subblock start;
- verify the allowed fallback path builds deterministically from the previous
  dependent snapshot/state instead of guessing or resetting;
- measure metadata overhead.

Inputs:

```text
v3_subblock_snapshot_check file1.wav [file2.wav ...]
```

Checks:

1. Encode whole file once with grid snapshots.
2. For each snapshot, load it into `GOSoundCompressionCacheV3`.
3. Decode `subblockSize + POLYPHASE_POINTS` samples or until section end.
4. Compare every decoded sample with source PCM.
5. For random positions inside each subblock, compare against
   `DecompressTo(position)`.
6. For subblock start `P`, assert live state means:

```text
m_position == P
first DecompressionStep() produces sample[P]
```

7. Verify beginning-of-section history is all zero.
8. For direct jump targets inside a render subblock:
   - compute `subblockStart = floor(target / subblockSize) * subblockSize`;
   - load the exact `SubblockStartSnapshot` for `subblockStart`;
   - decode to the jump target and compare with source PCM.
9. Simulate the "no exact snapshot" fallback:
   - load the previous dependent snapshot;
   - decode forward to `subblockStart - 1`;
   - assert the live state at `subblockStart` is exact;
   - decode to the jump target and compare with source PCM.

Output:

```text
file frames channels subblockSize snapshots metadataBytes metadataBytesPerFrame
decode ns/channel-sample
OK / failure with exact position/channel
```

Failure messages must include:

```text
file
subblockStart
position
channel
snapshot m_position
expected sample
actual sample
```

Acceptance:

- Runs on the same real files used by `v3_production_roundtrip_check.cpp`.
- All tested files pass for 64, 128, 256, and 512.
- Any predictor state bug fails near the boundary, not only after a long drift.
- Jump target P never starts from assumed zero-state unless its subblock start
  is the real section start.

Current result, run on `/tmp/batch8.txt`, `/tmp/batch8b.txt`, and
`/tmp/batch8c.txt` (24 real stereo WAVs):

```text
96/96 file/subblock-size checks passed
6 direct jump targets per file/subblock-size
6 deterministic fallback targets per file/subblock-size
subblock sizes: 64, 128, 256, 512
guard samples per subblock: 8
metadata/frame:
  subblock 64  -> ~0.688 B/frame
  subblock 128 -> ~0.344 B/frame
  subblock 256 -> ~0.172 B/frame
  subblock 512 -> ~0.086 B/frame
decode range in these runs: mostly ~0.73-1.07 ns/channel-sample, with one
short-run outlier at 1.137 ns/channel-sample
```

## Step 5: model GO segment playback in a tool

Add a second tool after Step 4 passes:

```text
tools/compression/v3_segment_subblock_check.cpp
```

Status 2026-07-07: implemented as a synthetic harness.

Purpose:

- model GO's MainSegment / EndSegment / StartSegment behavior without touching
  the real audio hotpath;
- verify render subblocks can represent loop boundaries and guard samples.

This tool should construct synthetic segment data first, then optionally reuse
real WAV loop metadata later.

Synthetic fixture requirements:

- stereo and mono cases;
- at least two loops in one file;
- overlapping or adjacent loop markers if representable;
- one loop with crossfade;
- file/release end with zero read-ahead;
- source positions deliberately not aligned to subblock boundaries.

Model data:

```text
MainSegment PCM
StartSegment offsets
EndSegment objects:
  transition_position
  end_pos
  next_start_segment_index
  prepared end_data bytes/samples
```

Checks:

1. Current GO-style scalar reference:
   - read MainSegment until `transition_position`;
   - read EndSegment from `transition_position` to `end_pos`;
   - jump to `next_start_segment_index + overflow`;
   - repeat.
2. Subblock model:
   - request currentSubblock;
   - request nextSubblock;
   - near loop end request jumpTargetSubblock;
   - read from subblocks with the same source positions.
3. Compare output sample-for-sample with scalar reference.
4. Verify polyphase guard samples:
   - near loop end, guard-after comes from current EndSegment continuation;
   - near file/release end, guard-after is zero;
   - no read crosses a subblock boundary unless guard/next is available.

Acceptance:

- Exact PCM equality for nearest-neighbor/source-window tests.
- A separate 8-tap FIR simulation matches between reference and subblock model.
- MainSegment and EndSegment have separate SegmentIds, and EndSegment ids are
  checked against the current playback state.
- EndSegments read directly from prepared `end_data`; the tool counts prepared
  samples that intentionally differ from MainSegment PCM.
- Multiple EndSegments with the same virtual `transition_position`/`end_pos`
  remain distinguishable by SegmentId.
- `transition_position` and `end_pos` are deliberately subblock-unaligned.
- Loop-end guard samples come from the current EndSegment continuation.
- Terminal guard samples are zero-filled.
- `jumpTargetSubblock` is requested before the modeled jump, not after it.
- Loop jump with overflow is tested explicitly:

```text
source >= end_pos
target = startOffset(next_start_segment_index) + overflow
```

Current result:

```text
9/9 segment/subblock checks passed
fixtures:
  stereo-two-loop-overflow
    two StartSegment/EndSegment loop path
    prepared EndSegment data
    loop-start guard continuation
    explicit overflow jumps
    jumpTargetSubblock requests
  same-virtual-position-endsegments
    two EndSegments share the same virtual transition/end positions
    prepared end_data differs per EndSegment id
    validates that cache keys cannot collapse EndSegments by position alone
  mono-file-end-zero-guard
    terminal EndSegment
    zero guard after file/release end
subblock sizes: 32, 64, 128
FIR taps: 8
```

## Step 6: prototype decoded subblock cache

Build this in a tool first. Do not wire into `GOSoundStream` yet.

Status 2026-07-07: implemented as
`tools/compression/v3_subblock_cache_check.cpp`.

Required cache entry state:

```cpp
enum class EntryState : uint8_t {
  Building,
  Ready,
  Failed,
};

struct DecodedSubblockEntry {
  GODecodedSubblockKey key;
  EntryState state;
  unsigned generation;
  unsigned channels;
  unsigned samplesIncludingGuard;
  unsigned validSamplesWithoutGuard;
  // layout-dependent storage
};
```

Absence from the map is the only "Empty" state. Once an entry object exists,
it must be exactly `Building`, `Ready`, or `Failed`.

Required cache key fields:

```text
SegmentId
layout
subblock start source position
valid render samples
guard samples
```

`SegmentId`, layout, and guard length are part of identity. Two entries with
the same virtual source position but different EndSegment id, output layout, or
guard length must not alias.

Prototype storage layouts:

Scalar planar:

```text
decoded[channel][sample]
```

Batch lane-interleaved:

```text
decoded[channel][sample * LANES + lane]
```

Cache rules:

- A ready entry is immutable.
- A ready entry must not be overwritten by a late duplicate build.
- A building entry may be duplicated by another worker if waiting would risk
  the audio callback. Duplicate work must be bounded per entry, not merely
  bounded by the small synthetic key space.
- Failed entries are distinct from `Ready` and `Building`; later requests must
  observe the failed state instead of rebuilding or waiting forever.
- Failed entries must carry a diagnostic reason in production code; the current
  tool only verifies state separation and counters.
- Production hotpath must not allocate a large buffer while holding a global
  lock.

Prototype policies to test:

1. Blocking miss resolution.
2. Duplicate build allowed.
3. Try-lock + synchronous fallback.

Metrics:

```text
requests
hits
misses
duplicateBuilds
duplicateWaits
syncFallbacks
readyOverwriteAttempts
failedResults
largeBuildsUnderMapLock
buildNs/channel-sample
consumeNs/channel-sample
maxWaitNs
```

Acceptance:

- Race simulation with N worker threads produces correct samples.
- Duplicate work is bounded per cache entry.
- No cache entry is mutated after `Ready`.
- Ready overwrite attempts are rejected and instrumented.
- `Building`, `Ready`, and `Failed` states are cleanly separated.
- No large decoded buffer build happens under the global map lock.
- `syncFallbacks` are counted separately from normal misses/builds.
- Cache-key separation is tested for `SegmentId`, layout, and guard length.

Current result:

```text
5/5 cache policy checks passed
workers: 8
requests per worker: 96
policy checks:
  blocking miss resolution
  duplicate-build allowance with per-entry bound
  try-lock synchronous fallback instrumentation
contract checks:
  key-contract: SegmentId/layout/guard separation and Ready overwrite rejection
  failed-state: Failed entry remains Failed and is not rebuilt
invariants:
  requests=768 per race policy
  misses=24 per race policy
  largeBuildsUnderMapLock=0 in every race policy
  Ready overwrite attempt rejected and instrumented
  Failed results instrumented
```

The exact hit/wait/duplicate/fallback counts are timing-dependent in this race
harness; the invariant is the bounded duplicate count, zero large builds under
the map lock, clean failed-state behavior, key separation, and 5/5 pass result.

This is still synthetic. It proves the counters and policy mechanics exist;
it does not prove real GO scheduling behavior or acceptable hotpath cost.

## Step 7: subblock builder APIs

Define builder APIs before production integration.

Status 2026-07-07: implemented as
`tools/compression/v3_subblock_builder_api_check.cpp`.

Scalar main-segment V3 builder:

```cpp
BuildMainV3Scalar(
  const GOSoundAudioSection &section,
  const GOSoundV3SubblockStartSnapshot &snapshot,
  unsigned samplesIncludingGuard,
  ScalarPlanarOut out);
```

Batch main-segment V3 builder:

```cpp
BuildMainV3BatchInterleaved(
  LaneBuildRequest requests[LANES],
  unsigned numLanes,
  unsigned samplesIncludingGuard,
  BatchInterleavedOut out);
```

EndSegment builder:

```cpp
BuildEndSegmentScalarOrInterleaved(
  const GOSoundAudioSection::EndSegment &end,
  unsigned subblockStartSourcePosition,
  unsigned samplesIncludingGuard,
  ...);
```

EndSegment builder initially copies/converts from uncompressed `end_data`.
Do not compress EndSegments in the first implementation.

Batch builder rules:

- MainSegment V3 requests can be grouped by:
  - channel count;
  - layout;
  - sample count including guard;
  - compatible batch lane requirements.
- EndSegment requests may be filled separately; do not force them through the
  compressed V3 batch decoder.
- Partial batches must use independent padding copies, exactly like
  `GOSoundCompressionCacheV3Batch` does today.

Acceptance:

- Builder output matches source PCM/reference for all segment kinds.
- Batch interleaved output matches scalar planar lane-by-lane.
- Partial batches are tested with 5 of 8 lanes.

Initial result:

```text
builder API checks passed
main builders:
  real WAV input from /tmp/batch8.txt, /tmp/batch8b.txt, /tmp/batch8c.txt
  scalar MainSegment V3 builder vs batch lane-interleaved MainSegment V3
  lanes=8 channels=2 samples=136
partial main builders:
  lanes=5/8 channels=2 samples=136
end builders:
  synthetic uncompressed EndSegment data
  scalar planar copy vs lane-interleaved copy
  lanes=8 channels=2 samples=136
```

## Step 8: production V3 format plumbing

Only after tools pass.

Status 2026-07-07: preflight contract implemented as
`tools/compression/v3_cache_format_contract_check.cpp`. The shared production
constants live in
`src/grandorgue/sound/playing/GOSoundAudioSectionCacheFormat.h`, so the tool
and the loader path use the same magic/version/codec values.

First production loader slice is implemented in
`src/grandorgue/sound/playing/GOSoundAudioSection.cpp`:

- old legacy cache-section bytes still load by treating the first `uint32` as
  `m_AllocSize` when the section-local magic is absent;
- new section-local headers are recognized;
- new-header `None` and `Legacy` codecs map back to the existing
  `m_IsCompressed` bool;
- new-header `V3`, future versions, invalid codec values, and nonzero flags
  are rejected cleanly until the V3 data path is wired;
- `SaveCache()` still writes the legacy prefix byte-for-byte when V3 is not
  enabled.

This step is mostly load/cache-build-time work, not audio-callback work:

- choosing the compression codec for a section;
- encoding main data as legacy or V3;
- generating V3 StartSegment snapshots and render-subblock snapshot-grid
  metadata;
- serializing/deserializing section metadata;
- rebasing stored offsets to real pointers after cache load.

It is still high-risk because cache-file compatibility bugs can silently make
old organs rebuild, fail to load, or load with mismatched segment metadata.

`GOSoundAudioSection` changes:

- replace or extend `m_IsCompressed` with an explicit codec version:

```cpp
enum class CompressionCodec : uint8_t {
  None,
  Legacy,
  V3,
};
```

or equivalent cache-file-safe discriminator.

- add `CompressV3()`;
- store V3 main data;
- store V3 StartSegment snapshots if needed for legacy stream compatibility;
- store V3 subblock snapshot grid metadata;
- serialize/deserialize all new metadata with versioning;
- keep old caches readable.

Important constraints:

- Current `StartSegment` is serialized with raw `sizeof(StartSegment)`.
- `GOOrganController::GenerateCacheHash()` already includes
  `sizeof(GOSoundAudioSection)` and `sizeof(GOSoundAudioSection::StartSegment)`,
  so many struct-layout changes will invalidate existing cache files instead
  of reading them with the wrong shape. Do not rely on that as the only version
  mechanism.
- Prefer a section-local explicit cache header for the new format:

```text
uint32 magic/version marker
uint16 section-cache-version
uint8  compression format: None / Legacy / V3
uint8  flags
then explicit fields
```

- The new reader should be able to read old bytes by treating the first
  `uint32` as `m_AllocSize` when the section-local magic is absent.
- The legacy writer path must stay byte-for-byte identical when V3 is disabled.
- Old binaries are not expected to read newly written V3 cache sections; the
  cache hash should keep those files separate. New binaries should still handle
  old cache-section bytes or rebuild cleanly.
- EndSegments remain uncompressed initially and must keep their existing
  serialization behavior unless a separate, tested EndSegment compression plan
  exists.

Preflight acceptance:

- Legacy-prefix bytes are byte-stable.
- New-compatible reader recognizes legacy compressed and uncompressed prefixes.
- V3 prefix carries explicit magic/version/codec fields.
- Future versions, invalid codec values, and truncated prefixes are rejected.

Initial preflight result:

```text
legacy compressed: OK bytes=33
legacy uncompressed: OK bytes=33
V3 explicit header: OK bytes=40 magic=0x33415347 version=1
production loader policy: OK accepts new None/Legacy, rejects V3 until payload support exists
negative cases: OK future-version invalid-format truncated
cache format contract checks passed
```

Current production compile check:

```text
make -C build/src/grandorgue sound/playing/GOSoundAudioSection.cpp.o
```

passed after rebuilding `GOSoundAudioSection.cpp`.

Acceptance:

- Legacy cache files still load.
- New V3 cache files roundtrip. Not complete yet; the current production
  loader deliberately rejects V3 headers until V3 payload metadata is written
  and tested.
- V3 can be disabled by setting/toggle.
- V3 fallback to legacy is possible without rebuilding the organ cache.

## Step 9: single-voice stream correctness path

Before decoded subblock cache replaces reads, V3 must be able to play through
the existing stream semantics.

Goal:

- prove V3 metadata can preserve GO loop and seek behavior;
- not yet chase full performance.

Changes:

- template or duplicate `StreamCacheWindow` / `StreamCacheReadAheadWindow` for
  V3 cache type; current code has already been mechanically
  template-parameterized on the cache type and still defaults to
  `GOSoundCompressionCache`, but no V3 stream path is wired yet;
- dispatch `getDecodeBlockFunction` by codec version;
- implement `GoToStartSegment` for V3 snapshots;
- keep EndSegment path uncompressed;
- keep old scalar renderer.

Tests:

- existing compression tools;
- new stream-level harness if available;
- manual real-organ A/B with V3 opt-in.

Acceptance:

- MainSegment playback is correct.
- StartSegment jumps are correct.
- EndSegment playback remains byte-identical to legacy behavior.
- Backward seek/release-align history does not corrupt V3 state.

This step is useful for correctness but is not the final performance design.
Do not stop here and call the codec-speed plan complete.

## Step 10: integrate decoded render subblock cache

After Step 9 correctness and Step 6 prototype.

Production cache responsibilities:

- request current subblock;
- request next subblock;
- near loop end request jumpTargetSubblock;
- expose scalar planar read views first;
- later expose batch lane-interleaved views.

Sampler state should include at least:

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

Hotpath behavior at loop boundary:

```text
switch segment state
attach already prepared jumpTargetSubblock
choose next EndSegment for future pass
schedule future current/next/jumpTarget subblocks
```

Avoid:

```text
constructing EndSegments
large cache lookups under locks
heavy synchronous decoding
inner-loop SIMD scatter to split per-lane buffers
```

Acceptance:

- Cache hit path has no large allocation.
- Loop boundary normal path does not decode synchronously.
- Synchronous fallback counter stays zero in normal benchmark runs.
- Output matches existing GO for looped samples.

## Step 11: batch subblock build

Only after scalar subblock cache is correct.

Batch miss grouping:

```text
collect up to 8 MainSegment V3 cache misses
load each lane's SubblockStartSnapshot
DecodeBatchInterleaved(...)
publish one batch cache entry or per-lane views backed by the batch entry
```

Open design choice:

1. Store one physical batch entry with lane mapping.
2. Publish eight logical subblock entries pointing into one batch storage.

Initial recommendation:

- prototype both in the tool;
- production likely wants logical entries pointing into one refcounted batch
  storage object, so scalar/batch consumers can ask by normal subblock key.

Correctness risks:

- lane mapping lifetime;
- partial batches;
- one lane failing while others succeed;
- eviction while a renderer still holds a view;
- mixing MainSegment and EndSegment lanes accidentally.

Acceptance:

- Batch-built entries compare bit-exact with scalar-built entries.
- Partial 5-lane batch is covered.
- Eviction/refcount test proves no dangling view.

**Status update 2026-07-08 (interleaved-storage fix, commit d47f08f44):**
Per-voice redesign (commit 9cebf585a, see project_codec_speed_investigation
memory) replaced the shared batch cache with private `VoiceBlock`s, but the
first cut of `GOSoundStream::BuildBlocksBatch` called the SPLIT-output
kernel (`DecodeBatch`, up to 8 scattered scalar stores) instead of this
step's `DecodeBatchInterleaved`. Production pro  filing (PCM: 95-97% CPU
with the redesign vs 69% pre-investigation baseline) plus the user's
explicit correction ("DU SOLLST DEN DECODERPATH NEHMEN, DER INTERLEAVED
SPEICHERT") traced back to exactly this: the split path throws away the
single-contiguous-store property that made the original benchmark fast.
Fix realizes design choice 2 above in its simplest form - not a
refcounted persistent batch storage object (no eviction/lifetime problem
exists anymore, per-voice blocks don't need one), just a per-call
temporary interleaved scratch buffer (`int32_t[channel][TOTAL*LANES]`)
that `BuildBlocksBatch` decodes into and immediately de-interleaves into
each voice's own private `m_Block`. Bit-exact against both existing tests
(`TestVoiceBlockBatchMatchesLazy`, `TestVoiceBlockPredictedRebuildMatchesUncompressed`).
Real-world PCM re-measurement against Legacy is still outstanding (user
to re-test).

## Step 12: renderer consumption decision

Use Phase 2.5 numbers, but remeasure in production context.

Variants:

1. Existing scalar renderer reads scalar planar subblocks.
2. Existing scalar renderer reads one lane from lane-interleaved batch
   subblocks.
3. Batch renderer consumes lane-interleaved subblocks in sample-major order.

Decision rule:

- If scalar lane-interleaved read remains close enough and avoids duplicate
  cache storage, one layout may be acceptable.
- If scalar lane-interleaved read loses too much bandwidth/cache efficiency,
  keep two layouts:
  - scalar planar cache for existing renderer;
  - batch lane-interleaved cache for batch decode + batch render.

Do not decide from decoder speed alone.

**DECIDED 2026-07-09 (variant 3, by PCM measurement, not decoder speed):**
after the callback-aligned redesign fixed batch fill (7.65/8 lanes, 0.2%
solo, instructions 197G->110G), the engine was still memory-bound (IPC
1.12->0.67, L3MPI x3): the per-voice decoded-block intermediate stage
(de-interleave write + render re-read, ~2x10MB/period at 13.6k voices)
was the remaining traffic vs legacy's 1KB L1-resident ring buffer. So:
a batch RENDERER (GOSoundResampleBatch.h - lane=voice, per-lane gathers
from the interleaved decode scratch since the lanes' positions drift
apart after the callback-aligned start, explicit FMA taps, vectorized
position update, fused fader gains g0+j*dg with a fixed hsum tree
accumulating all 8 voices into the output at once) consumes the decode
kernel's interleaved scratch DIRECTLY (GOSoundStream::
DecodeGroupToScratch - no VoiceBlock persistence on this path), driven
per claimed batch by GOSoundGroupTask::ProcessVoices with a three-way
classification (GOSoundStream::ClassifyForRender): BatchRender (plain
forward callback - the fused path), BuildOnly (wraps/section
boundaries - classic path with a prebuilt block), None (classic).
GOSoundFader::ComputeFrameGains extracts the linear gain state machine
so batch and classic paths share it; non-linear crossfade/ToneBalance
voices ride along in the render group (zero-gain lane + laneOut
extraction) and are faded classically - the accumulate kernel already
has a gain-buffer input (variant b) wired for batching them later.
Two hard-won FP lessons pinned in tests: gcc contracts even INTRINSIC
mul+add to FMA under -ffp-contract=fast, so the kernel uses EXPLICIT
_mm256_fmadd_ps (reference: std::fmaf) and pins fp-contract=off per
function; and the test's scalar reference must live in its own
noinline/pinned function or the optimizer re-fuses it (heisenbug:
adding a debug printf made the test pass).

Acceptance:

- production-like benchmark reports decode, consume, and combined pipeline
  time;
- benchmark includes loop/endsegment cases, not only straight main data;
- decision is documented in `INVESTIGATION.md`.

## Step 13: instrumentation

Add counters before performance claims.

Suggested counters:

```text
subblockRequests
subblockHits
subblockMisses
subblockBuilds
duplicateBuilds
syncFallbacks
jumpTargetPrepared
jumpTargetMissingAtBoundary
endSegmentSubblockBuilds
mainSegmentSubblockBuilds
batchBuilds
partialBatchBuilds
evictions
```

Audio monitor visibility later:

- cache hit rate;
- sync fallback count;
- duplicate build count;
- average/max subblock build time;
- batch occupancy.

Do not add UI first. Add counters/logging first.

## Step 14: final performance validation

Required benchmark sets:

- straight one-shot samples;
- single sustain loop;
- multiple loops;
- release samples with alignment;
- real full organ registration/chord workloads;
- high-polyphony stress.

Required comparisons:

- legacy compressed path;
- V3 direct single-voice path;
- V3 scalar subblock cache;
- V3 batch subblock build + scalar consume;
- V3 batch subblock build + batch consume.

Report:

```text
decode/build ns/channel-sample
consume ns/channel-sample
combined ns/channel-sample
cache hit rate
duplicate build rate
sync fallback count
memory footprint
```

No speed claim is final unless it includes cache hit/miss behavior.

## Stop conditions

Pause implementation and revisit design if any of these happen:

- subblock boundary output differs from scalar `DecompressTo()`;
- a jump target subblock would need guessed/zero decoder state away from true
  section start;
- loop boundary output differs from existing GO;
- a predictor mismatch only appears after several samples of drift;
- EndSegment data is accidentally treated as MainSegment compressed data;
- hotpath loop transition needs allocation as normal behavior;
- sync fallback is common in normal playback;
- lane-interleaved layout helps decode but loses the win in renderer consume;
- metadata overhead becomes unacceptable for realistic sample sets.

## Immediate next task

Step 0 and Steps 4-8 preflight are now completed at tool/prototype level:

```text
v3_subblock_snapshot_check:
  /tmp/batch8.txt, /tmp/batch8b.txt, /tmp/batch8c.txt
  96/96 file/subblock-size checks passed
  direct jump targets and deterministic fallback targets covered
v3_segment_subblock_check: 9/9 segment/subblock checks passed
v3_subblock_cache_check:
  5/5 cache policy checks passed
  cache-key separation covers SegmentId, layout, and guard length
  Ready immutability, Failed-state separation, bounded duplicate builds,
  syncFallback instrumentation, and no-large-build-under-map-lock covered
v3_subblock_builder_api_check:
  /tmp/batch8.txt, /tmp/batch8b.txt, /tmp/batch8c.txt
  builder API checks passed
v3_cache_format_contract_check:
  cache format contract checks passed
```

Status update 2026-07-08 (slice 3): steps 10 and 11 are now implemented in
production following the two-phase-callback design recorded in
INVESTIGATION.md ("Design decision 2026-07-08"):

- `GOSoundDecodedSubblockCache`: one atomic slot per render subblock,
  lock-free Ready reads (`TryGetReadyView`), CAS-claimed entry ownership,
  release-publish, `BuildRuns()` (scalar run chains + 8-lane AVX2 batch
  wavefront with per-entry dependent end states and guard hand-over);
  the synchronous `GetSubblock()` miss path stays as the instrumented
  safety net. Waits escalate spin -> yield -> sleep (see INVESTIGATION.md
  for the Windows realtime-starvation lesson).
- `GOSoundStream::CollectNeededSubblocks`: exact decode-span prediction
  (ReadBlock's segment walk on a position copy, loop-jump target, one
  subblock of fixed lookahead).
- `GOSoundGroupTask`: two-phase period - Phase A plans (dedupe/coalesce/
  pack, first thread under the existing mutex) and builds (fetch_add
  batch claims, spin-then-condition phase barrier), Phase B renders
  lock-free through `StreamSubblockWindow`.
- Tests: `TestSubblockBuildRuns`, `TestTwoPhaseSpanPrediction` (plus all
  earlier stream/cache tests) pass; prediction keeps `syncBuilds == 0`
  across looped playback at pitches 1.0/1.25/0.8.

**Bounded-window rule (2026-07-08, user architecture decision after the
first real Windows load test):** a decoded cache that ACCUMULATES the
organ is nonsense - it pages and eventually kills the machine (observed:
system freeze under polyphony). The render subblock is the per-callback
decode window and must match the audio buffer length; per playing
position only the cursor subblock and its successor are normally alive,
plus the loop-jump target around a wrap (3 around wraps). After each
callback, fully played subblocks are unlinked. Implemented as: an
inactivity sweep (once per period, organ-wide registry, entries untouched
for >= 2 sweep ticks get unlinked), TYPE-STABLE recycling (unlinked
entries go to a per-cache pool and are reused by later claims - memory is
never freed under a reader; acquired Views hold reader references and the
speculative acquire/re-validate protocol makes recycling safe without
locks on the read path), and live-entry/byte gauges in the periodic log.
The subblock size is currently a constant 128 matching the user's
configured audio buffer; following the engine's actual samples_per_buffer
is a planned follow-up.

**Grid-floor seed snapshots (2026-07-08, after the first bounded-window
load test):** the user's log showed healthy counters but multi-ms Phase A
stalls (build max 12.6ms / barrier max 9ms at a 2.67ms period) - the
subblock CONTAINING a loop start begins before the loop's own snapshot,
so its per-wrap rebuild forward-decoded from the section beginning.
CompressV3 now also captures a snapshot at every start segment's subblock
grid floor; all captured positions are serialized as seeds (section cache
version 2) and the build-state preparation searches them, bounding every
forward decode to at most one subblock. This is ~40 bytes per loop, NOT
the rejected whole-section grid.

**Parallel span collection + two build-environment bugs found and fixed
(2026-07-08, the 13600-voice load test):** at that scale the SERIAL
per-voice `CollectNeededSubblocks` walk (done by the period's first
thread alone) became the new bottleneck (log: plan=2240/11094us,
approaching the ~2.7ms period budget by itself). Two changes: (1)
`CollectNeededSubblocks` now filters out already-Ready subblocks itself
(touch-stamping them for the sweep) instead of emitting the whole span -
in steady state this is almost everything, so only genuinely new
subblocks reach the merge; (2) collection itself runs PARALLEL across
workers (Phase A0: claim batches of 64 voices via fetch_add, collect into
thread-local buffers, bulk-append under a small mutex; the worker
completing the last batch becomes the merger). Log gained a `collect`
timing bucket next to the now-narrow `merge` bucket.

This surfaced two SEPARATE, non-algorithmic bugs when the user tried a
clean win64 rebuild to test it - worth remembering, since neither was
about the subblock cache design itself:

1. **CMake cache hijacking.** `update-build-on-linux.sh` runs
   `cmake "$SRC_DIR" ...` (SRC_DIR = repo root) from within
   `build/win64`. `cmake <dir>` is ambiguous: if `<dir>` already contains
   a `CMakeCache.txt`, CMake treats it as an EXISTING BINARY dir to
   refresh (using ITS stored source-dir reference), completely ignoring
   the caller's cwd. An earlier in-source `cmake .` run at the repo root
   (for the Linux GOTestExe suite, meant for a separate `build/`
   subdirectory) left exactly such a stray cache at root. This stayed
   invisible because the script skips reconfiguring whenever
   `build/win64/CMakeCache.txt` already exists - only `--clean` (wiping
   that cache) forces a fresh `cmake $SRC_DIR` call and triggers the
   hijack, landing the whole configure at the repo root with a different,
   wrong wxWidgets (`/mingw64`-packaged 3.2.8, not the project's own
   from-source "_custom" build) and no Windows cross-compile flags applied
   (`WIN32` never becomes true there). Fix: removed all stray in-source
   CMake-generated files at the repo root (`CMakeCache.txt`,
   `CMakeFiles/`, `cmake_install.cmake`, etc. across the tree) - the repo
   root must stay pure source, never its own in-source build dir, or this
   silently breaks the win64 script again on the next `--clean`.
2. **`__cxa_thread_atexit` missing at runtime ("DLL entry point not
   found").** The parallel collection's scratch buffer was originally
   `static thread_local std::vector<Need>`. `std::vector` has a
   non-trivial destructor, so a thread_local of it needs
   `__cxa_thread_atexit` to register thread-exit cleanup - and this
   project's shipped `libstdc++-6.dll` (confirmed via `objdump -p`) does
   not export that symbol, while the freshly linked `GrandOrgue.exe`
   started importing it (confirmed via `objdump -p GrandOrgue.exe`,
   grepped against the DLL's export table). It was the FIRST thread_local
   with a non-trivial destructor anywhere in this codebase - every other
   thread_local (grep the whole tree) is a bool, a pointer, or a struct
   with no destructor, none of which need this symbol. Fix: a plain
   automatic-storage local instead (declared once before the claim loop,
   cleared/reused per iteration exactly as before - loses nothing within
   one `RunDecodePhase` call, just doesn't persist across periods, which
   was never needed). **Lesson: never introduce a `thread_local` with a
   non-trivial destructor in this codebase without verifying
   `__cxa_thread_atexit` against the actual shipped `libstdc++-6.dll`
   first** - the mingw-w64/custom-wx toolchain here does not reliably
   provide it, and the failure mode is a silent link-time success but a
   hard runtime crash on every real Windows machine.

**SUPERSEDED (2026-07-08, per-voice redesign) - everything above this
point about the shared/atomic-slot/pool/sweep decoded-subblock cache, its
bounded-window eviction rule, and GOSoundGroupTask's organ-wide two-phase
collect/merge/barrier machinery is HISTORY, not the current design. Kept
for the record of how the bugs it caused (RAM accumulation, per-wrap
decode stalls, the collect-phase bottleneck at 13600 voices) were found
and reasoned about, but none of that machinery exists in the code
anymore.**

**Per-voice decoded render blocks (2026-07-08, user architecture
correction):** the shared cache was solving a problem that cannot occur.
A real pipe never sounds twice at once, so no two simultaneously active
voices ever need to coordinate over the same decoded window - the
deduplication the shared design existed for, and everything built to
make sharing safe under concurrency (CAS claim, reader refcounting, pool
recycling, inactivity-sweep eviction, the global sort/merge that found
duplicate needs across thousands of voices), was solving a non-problem
and became the next real bottleneck itself.

New design: every `GOSoundStream` owns a small decoded render block
directly (`SUBBLOCK_SAMPLES` + `GUARD_SAMPLES` per channel, plus the
exact decoder state right after its last valid sample for dependent-
forward chaining - the same no-grid state-source rule as before, just
private per voice). `GOSoundGroupTask` no longer runs a separate decode
phase with an organ-wide barrier: the SAME worker that already claimed a
batch of voices for rendering (the pre-existing `VOICE_BATCH_SIZE` claim)
does two purely LOCAL passes over that same batch - build
(`GOSoundStream::NeedsBlockRebuild` + `BuildBlocksBatch`, grouping same-
channel-count voices into up to 8 lanes for the unchanged AVX2 kernel)
then render (the unchanged `ProcessSampler` loop) - zero cross-worker
coordination, since no other worker ever touches these voices' blocks. A
mid-callback subblock crossing the decode pass didn't predict (high
pitch, or a loop wrap landing mid-callback) is rebuilt locally and
synchronously right inside the render window - safe and cheap because
the block is private, unlike the old design's cross-thread fallback.

`GOSoundDecodedSubblockCache` (shared cache, pool, sweep, registry) is
deleted outright. `GOSoundAudioSection` keeps only what building a
private block still needs: the subblock seed snapshots (`CompressV3`'s
grid-floor seeding is UNCHANGED - it was never part of the removed
machinery) and `FindV3SnapshotAtOrBefore`. The subblock grid constant
moved to a tiny standalone header (`GOSoundSubblockGrid.h`) so the
section (seed generation) and the stream (runtime) share it without an
unwanted direction of dependency between them.

Tests: the four cache-specific tests are replaced by two end-to-end
tests against the new private-block API -
`TestVoiceBlockBatchMatchesLazy` (the AVX2 batch path, including the
short-final-subblock scalar-fallback branch mixed into one batch call,
must match a voice that only ever gets rebuilt lazily) and
`TestVoiceBlockPredictedRebuildMatchesUncompressed` (the decode pass's
rebuild, at several pitches through loop wraps, must match an
uncompressed reference bit-exactly). All pre-existing stream/cache-
roundtrip tests pass unchanged - the public `GOSoundStream` API
(`SetUseSubblockCache`/`ReadBlock`) they drive stayed stable throughout
the redesign. Both Linux test build and the full app (Linux and win64)
compile clean; the `__cxa_thread_atexit` build bug is gone along with it
(the `thread_local` scratch buffer it came from was part of the removed
collect phase).

**Status update 2026-07-09 (resampling-ratio bins + segment-anchored
grid):** production measurement of the interleaved-storage fix (PCM: UTIL
0.90, L3 hit 72%, callback time ~= compute time at high polyphony) led to
the user's diagnosis that the interim de-interleave transpose in
BuildBlocksBatch doesn't pay off against the still-scalar render consumer,
and to the decision: bins first, then the 8-lane render kernel (which will
consume the interleaved layout directly and remove the transpose). The bin
redesign landed as:

- **Bin ladder** (`GOSoundSubblockBins`, GOSoundSubblockGrid.h): base =
  the engine's REAL samples_per_buffer (closes former still-open item 2),
  rungs grow by ceil(*1.02) (user's 2%-max-overdecode rule; doubling was
  considered and rejected - up to ~50% waste), top rung = 4x base (~2
  octaves of repitch incl. source/device rate mismatch). Each voice picks
  its rung at note start from its exact resampling increment (integer
  math, includes tuning + random detune); beyond the top rung the voice
  OPTS OUT to the direct V3 path at note start (capping would mean a miss
  every callback - user's explicit correction).
- **Segment-anchored grid** replaced the global position-0 modulo grid:
  each voice's blocks tile [anchor + k*bin) with anchor = current segment
  start (set at note start, release alignment, every wrap). A post-wrap
  block therefore begins EXACTLY at the loop start, where a
  StartSegmentV3 snapshot exists - so CompressV3's grid-floor seeds
  became unnecessary and were removed (cache format version 2 kept: same
  structure, old caches' extra floor seeds load fine, merely redundant).
  This also freed the bin length to be per-VOICE (not per-section) and
  made the disk cache independent of the buffer-size setting. The render
  window's hot path got cheaper, not costlier: a plain range check
  (index - subblockStart < validSamples) instead of a modulo; the
  anchored-grid division runs only on the rare rebuild path.
- **Storage**: VoiceBlock's compile-time array became a pool-owned buffer
  (GOSoundSamplerPool owns one per pooled sampler, sized channels x (4*
  base + guard), restored around GetSampler()'s memset - an owning
  pointer inside the stream would leak there). Memory scales with the
  sampler pool, not the sample library.
- **Bin-homogeneous batches**: DrainIntoEpoch stable-bucket-sorts the
  per-period flat array by (binIndex, channels) - single-threaded, O(n),
  allocation-free at steady state; the fetch_add claim machinery is
  untouched (this realizes the user's bin-claiming design in its simplest
  form). RunSubblockDecodePass groups lanes by that same key; a batch
  kernel group must share the bin, since lanes step in lockstep.
  BuildBlocksBatch decodes through a fixed 256-sample interleaved chunk
  scratch (bins are runtime-sized now, stack stays bounded for any
  buffer setting).
- Tests extended: pitches across the ladder incl. an opt-out case (4.5x)
  that must still play bit-exactly via the direct path, and an assertion
  that the first rebuild after a wrap starts exactly at the loop start.
  12/12 green.

Still open (next tasks, roughly in order):

1. Real-organ Windows load test with the per-voice redesign (history:
   the shared-cache design went through RAM-accumulation, per-wrap
   decode-stall, and collect-phase-bottleneck failures in sequence before
   being replaced outright; this is the first test of the architecture
   that removes the shared state those bugs lived in, not just another
   fix on top of it - watch the "V3 per-voice decode" log line, process
   RAM, and for glitches at high polyphony). With the bins in, also watch
   soloRebuilds vs batchCalls - bucket sorting should keep groups full.
2. DONE 2026-07-09: subblock size follows the engine's real
   samples_per_buffer (bin ladder base; see status update above).
3. Future optimization (user, 2026-07-08, explicitly deferred): build the
   NEXT loop-jump target right after a wrap (once `PickEndSegment`'s
   `rand()` has fixed the next start segment) instead of just-in-time
   before the next wrap - a voice has many callbacks of slack before it
   actually needs it. Simpler to add now than before: no sweep/touch-
   stamping concern anymore (the block is private, nothing evicts it), so
   this is purely "predict one loop pass further ahead in
   NeedsBlockRebuild", not a lifetime-management change.
4. Render-side batching (user, 2026-07-08; lane semantics clarified same
   day): a future resample kernel operating on 8 voices' m_Block data at
   once - LANE=VOICE, matching the decode kernel (a common point of
   confusion this session: LANES are NOT one-lane-per-WAV-channel; each
   lane is one independent voice's full state, and for multi-channel
   voices the channel loop is OUTER/sequential, one `__m256i` per channel
   iteration covering all 8 voices). For stereo sample sets (the common
   case) this means 8 voices = 4 stereo pairs get batched together, same
   grouping the user originally described as "4 stereo voices". The
   render kernel would read sequentially over FIR-tap-index rather than
   sample position, one contiguous vector load per tap across the 8
   voices - which is exactly the layout `DecodeBatchInterleaved` now
   writes (see Step 11 status update above), so decode and render staying
   separate passes over the SAME claimed batch (already the case:
   GOSoundGroupTask::ProcessList runs RunSubblockDecodePass over a
   claimed batch, then renders that same batch in a second loop) means
   the render kernel could read decode's interleaved output directly
   without a re-layout step, IF it operates on the same 8-voice grouping.
   Not yet designed in code detail or implemented - deliberately deferred
   until decode-side measurement (item 5) is in.
5. Measure - decode-pass cost per callback at high polyphony, rebuild
   rate / batch-vs-solo ratio in real playback (the "V3 per-voice decode"
   log line), decode+consume ns/channel-sample vs the direct V3 and
   legacy paths. No cache hit rate or sync-wait counters anymore - there
   is no shared state left to report on; a "miss" is now just "this
   voice's own block needs a rebuild", always resolved by whichever
   worker owns that voice this period, never a wait on another thread.
6. Instrumentation visibility beyond the log line (audio monitor).
7. GOSoundTremulantTask voices (wave tremulants) do not go through
   GOSoundGroupTask::ProcessList's decode pass, so they only ever use the
   lazy, local, synchronous rebuild inside the render window - correct
   (per-voice ownership makes this always safe now, unlike the old shared
   design's synchronous-fallback caveat), just never AVX2-batched. Decide
   whether that matters enough to give tremulant tasks their own decode
   pass, or leave it as an accepted, correct, unbatched path.
8. Slack-based decode-ahead (user idea, 2026-07-09, explicitly deferred):
   real-time audio survives OS/other-process load bursts best when quiet
   callbacks bank work in advance - measure the remaining time to the
   deadline at points within a callback, and while there is spare margin,
   decode further blocks ahead (subblock-granular, abortable after every
   block as the margin shrinks). The dependent-forward rule
   (m_BlockEndState) already supports sequential build-ahead with no
   extra state sourcing; needed additionally: a callback-start timestamp
   + budget (samples_per_buffer / sample_rate), a margin check between
   batches, and a place to keep the pre-built next block (a second
   VoiceBlock slot per voice, or an ahead-flag on the existing one).
   Generalizes item 3 (next-wrap-target preparation): same family - use
   idle-callback slack for predictable future decode work - but driven by
   measured margin instead of the loop-wrap event alone.
