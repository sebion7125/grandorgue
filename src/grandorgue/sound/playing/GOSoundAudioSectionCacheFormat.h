/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDAUDIOSECTIONCACHEFORMAT_H_
#define GOSOUNDAUDIOSECTIONCACHEFORMAT_H_

#include <cstdint>

// Section-local cache envelope for future GOSoundAudioSection compression
// formats. Legacy cache sections do not start with this marker; a compatible
// reader treats the first uint32 as m_AllocSize when the marker is absent.
static constexpr uint32_t GO_SOUND_AUDIO_SECTION_CACHE_MAGIC = 0x33415347u;
/* Version 2: adds the V3 subblock seed snapshots (count + entries) after
 * the StartSegmentV3 block. Version-1 caches are rejected and rebuilt.
 *
 * Content note (2026-07-09 bin redesign, same structure, no bump): newly
 * written caches seed only the start segment offsets - the per-voice
 * render-block grid is segment-anchored now, so the former grid-floor
 * seeds are unnecessary. Older version-2 caches containing them load
 * fine; their extra seeds are valid, merely redundant resume points. */
// Version history:
//   1 - explicit V3 envelope: codec + StartSegmentV3 snapshots
//   2 - subblock seed snapshots (m_SubblockSeedsV3)
//   3 - optional uncompressed prefix (raw PCM head + prefix-end seed) so a
//       voice START never decodes (attack block 1; release skip + block 1)
//   4 - V3 predictor changed from damped 2nd-order (prev+((prev-last)>>1))
//       to undamped 2*prev-last: ~2-7% smaller residuals at equal/lower
//       decode cost. Changes the V3 residual stream -> old caches invalid.
//   5 - V3 variable predictor order: the encoder picks, per section, the
//       fixed order (1..4) giving the smallest cache; the order is stored
//       in each block header ([order][width], self-describing). Changes
//       the V3 block layout + residual stream -> old caches invalid.
//   6 - V3 fixed 3rd-order predictor (no per-block order byte); best
//       single-order compression + polyphony ceiling. Distinct residual
//       stream from v5 -> old caches invalid.
//   7 - V3 variable order 1..4 with uniform-order lane grouping: same
//       self-describing [order][width] layout as v5, but voices are sorted
//       by order (bucket key + render-batch flush) so each 8-lane batch is
//       uniform-order and decodes via one hardcoded per-order predictor.
//   8 - block-header layout now depends on the predictor mode: only Variable
//       needs the per-block order byte ([order][width]); the fixed modes emit
//       [width] alone, which is byte-for-byte the pre-v7 (v6) stream. This
//       keeps a fixed order's stream, size and data alignment exactly what
//       the hardcoded fixed-order build had. The mode is folded into the
//       cache hash, so the reader always knows which layout to expect.
//   9 - order 4 + Variable removed (see GOV3PredictorMode): the 4th history
//       tap left the snapshot and the live decode state, which is what the
//       polyphony ceiling actually pays for. Header is [width] again, i.e.
//       the v6 stream, and the per-voice state is v6-sized (432 B).
//  10 - the per-snapshot predictor order is gone: the order is global and
//       session-constant, so storing it made every snapshot 4 bytes bigger
//       (40 -> 44) - and snapshots are read on every voice start and block-
//       state rebuild, i.e. exactly the DRAM traffic the ceiling is bound
//       by. Measured ~7% per voice-frame against the build without it.
//       Snapshot and live state are now v6-sized again.
//  11 - order 4 returns, cost-isolated (2026-07-17 rebuild of the v7 idea
//       without its per-voice tax): the section envelope carries a
//       per-section predictor order byte (stage 1: always == the mode;
//       prepares per-section Variable), and the seeds' 4th history tap
//       (v[n-4]) is stored in SEPARATE parallel arrays written only when
//       order >= 4 - the Snapshot struct itself stays exactly 40 bytes,
//       so the fixed modes' snapshot stream and in-memory seed traffic
//       are bit-identical to v10. Block header stays [width] in every
//       mode (the order is section metadata, never per block).
static constexpr uint16_t GO_SOUND_AUDIO_SECTION_CACHE_VERSION = 11;

enum class GOSoundAudioSectionCompressionCodec : uint8_t {
  None = 0,
  Legacy = 1,
  V3 = 3,
};

/* V3 predictor selection. The numeric value IS the predictor order, so the
 * encoder forces it directly; the per-block header carries no order byte.
 *
 * Order 4 was removed 2026-07-15 (the 4th tap grew every voice's live
 * state 432 -> 456 under per-object heap allocation, costing ~1.25% of the
 * ceiling for everyone) and RE-ADDED 2026-07-17 with the cost isolated:
 *  - live state: m_hist3 sits at the END of GOSoundCompressionCacheV3, so
 *    every pre-existing field keeps its offset; orders <= 3 never touch it
 *    (if constexpr), and the arena slot stays 960 B (the sampler grows
 *    880 -> 896 = still exactly 14 cache lines). The old allocation-layout
 *    penalty (LFH bucket jump) no longer exists under the arena pool.
 *  - snapshots: the 4th tap lives OUTSIDE the 40-byte Snapshot struct in
 *    parallel arrays only order-4 sections populate (see cache v11), so
 *    the fixed modes' per-voice-start seed traffic is unchanged (this was
 *    v10's ~7% lesson).
 * Gate: the Fixed3 A/B (one binary, runtime switch, 0.7% resolution) must
 * show no regression - measured, not argued. */
enum class GOV3PredictorMode : uint8_t {
  /* Variable: the encoder picks the smallest of order 1..4 PER SECTION
   * (stage 2, 2026-07-17). The value 0 is deliberately NOT a valid order -
   * every decode entry point takes the real order from the section
   * (m_PredictorOrder); value 0 matches the v7-era config string so old
   * "Variable" configs keep working. Uniform-order render batches are the
   * scheduler's job (bucket-key order fold + group flush, both gated off
   * in the fixed modes). */
  Variable = 0,
  Fixed1 = 1,
  Fixed2 = 2,
  Fixed3 = 3, // default: best all-round compression + polyphony ceiling
  Fixed4 = 4, // best compression on smooth material; costs a 4th tap
};

inline bool IsValidGOSoundAudioSectionCompressionCodec(uint8_t codec) {
  switch ((GOSoundAudioSectionCompressionCodec)codec) {
  case GOSoundAudioSectionCompressionCodec::None:
  case GOSoundAudioSectionCompressionCodec::Legacy:
  case GOSoundAudioSectionCompressionCodec::V3:
    return true;
  }
  return false;
}

#endif
