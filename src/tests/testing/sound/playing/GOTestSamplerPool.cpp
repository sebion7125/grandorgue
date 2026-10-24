/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOTestSamplerPool.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <vector>

#include "sound/playing/GOSoundSampler.h"
#include "sound/playing/GOSoundSamplerPool.h"

const std::string GOTestSamplerPool::TEST_NAME = "GOTestSamplerPool";

namespace {

constexpr size_t LINE = 64;
// 8 ints per frame pair (2 channels * 4 bytes) -> stride 128 gives a
// 1024-byte payload: exactly the power-of-two worst case the odd-line
// slot rounding exists for.
constexpr unsigned CHANNEL_STRIDE = 128;

/* Drains every available sampler (up to the usage limit) and returns
 * the drawn pointers. The pool hands them out in freelist (LIFO) order,
 * not address order. */
std::vector<GOSoundSampler *> DrainPool(GOSoundSamplerPool &pool) {
  std::vector<GOSoundSampler *> out;

  for (GOSoundSampler *s; (s = pool.GetSampler()) != nullptr;)
    out.push_back(s);
  return out;
}

} // namespace

void GOTestSamplerPool::TestArenaGeometry() {
  GOSoundSamplerPool pool;

  pool.SetVoiceBlockChannelStride(CHANNEL_STRIDE);
  pool.SetUsageLimit(1000);

  // Slot contracts: covers the payload, whole lines, odd line count.
  const size_t samplerSlot = GOSoundSamplerPool::GetSamplerSlotSize();
  const size_t blockSlot = pool.GetVoiceBlockSlotSize();

  GOAssert(
    samplerSlot >= sizeof(GOSoundSampler) && samplerSlot % LINE == 0
      && (samplerSlot / LINE) % 2 == 1,
    GOFormat("bad sampler slot size {}", samplerSlot));
  GOAssert(
    blockSlot >= sizeof(int) * 2 * CHANNEL_STRIDE && blockSlot % LINE == 0
      && (blockSlot / LINE) % 2 == 1,
    GOFormat("bad voice block slot size {}", blockSlot));

  std::vector<GOSoundSampler *> drawn = DrainPool(pool);

  GOAssert(
    drawn.size() == 1000,
    GOFormat("usage limit gate: drew {} of 1000", drawn.size()));
  GOAssert(pool.GetSampler() == nullptr, "pool exceeded its usage limit");

  // Address contracts: alignment, fixed stride from the arena base.
  std::vector<uintptr_t> addrs;
  std::vector<uintptr_t> blockAddrs;

  for (const GOSoundSampler *s : drawn) {
    addrs.push_back((uintptr_t)s);
    GOAssert(
      s->stream.GetBlockData() != nullptr, "voice block missing after draw");
    blockAddrs.push_back((uintptr_t)s->stream.GetBlockData());
  }
  std::sort(addrs.begin(), addrs.end());
  std::sort(blockAddrs.begin(), blockAddrs.end());
  GOAssert(
    addrs[0] == (uintptr_t)pool.GetSamplerArenaBase(),
    "lowest sampler is not the arena base");
  GOAssert(
    blockAddrs[0] == (uintptr_t)pool.GetVoiceBlockArenaBase(),
    "lowest voice block is not the arena base");
  for (size_t i = 0; i < addrs.size(); i++) {
    GOAssert(
      addrs[i] % LINE == 0 && blockAddrs[i] % LINE == 0,
      GOFormat("slot {} not line-aligned", i));
    if (i > 0) {
      GOAssert(
        addrs[i] - addrs[i - 1] == samplerSlot,
        GOFormat("sampler stride broken at {}", i));
      GOAssert(
        blockAddrs[i] - blockAddrs[i - 1] == blockSlot,
        GOFormat("voice block stride broken at {}", i));
    }
  }

  // The point of the odd stride: 64 consecutive slots hit all 64 L1
  // set positions (address bits 6-11).
  std::set<unsigned> samplerSets, blockSets;

  for (unsigned i = 0; i < 64; i++) {
    samplerSets.insert((addrs[i] / LINE) % 64);
    blockSets.insert((blockAddrs[i] / LINE) % 64);
  }
  GOAssert(
    samplerSets.size() == 64,
    GOFormat("samplers cover only {} of 64 cache sets", samplerSets.size()));
  GOAssert(
    blockSets.size() == 64,
    GOFormat("voice blocks cover only {} of 64 cache sets", blockSets.size()));

  // Freelist round trip.
  for (GOSoundSampler *s : drawn)
    pool.ReturnSampler(s);
  GOAssert(pool.GetSampler() != nullptr, "pool empty after returning all");
  pool.ReturnAll();

  /* Growth: a second chunk appears, existing addresses stay valid (the
   * old chunk is never moved), and the full drain yields exactly two
   * constant-stride address runs - one per chunk. */
  pool.SetUsageLimit(1500);

  std::vector<GOSoundSampler *> drawn2 = DrainPool(pool);

  GOAssert(
    drawn2.size() == 1500,
    GOFormat("after growth drew {} of 1500", drawn2.size()));

  std::vector<uintptr_t> addrs2;

  for (const GOSoundSampler *s : drawn2) {
    addrs2.push_back((uintptr_t)s);
    GOAssert(
      s->stream.GetBlockData() != nullptr, "voice block missing after growth");
  }
  std::sort(addrs2.begin(), addrs2.end());

  unsigned runs = 1;

  for (size_t i = 1; i < addrs2.size(); i++)
    if (addrs2[i] - addrs2[i - 1] != samplerSlot)
      runs++;
  GOAssert(runs == 2, GOFormat("expected 2 arena runs, found {}", runs));
  GOAssert(
    std::includes(
      addrs2.begin(), addrs2.end(), addrs.begin(), addrs.end()),
    "growth moved existing samplers");

  // Stride change: every sampler gets reattached to a new arena.
  pool.ReturnAll();
  pool.SetVoiceBlockChannelStride(CHANNEL_STRIDE * 2);

  std::vector<GOSoundSampler *> drawn3 = DrainPool(pool);
  const size_t blockSlot2 = pool.GetVoiceBlockSlotSize();

  GOAssert(
    blockSlot2 >= sizeof(int) * 2 * CHANNEL_STRIDE * 2
      && (blockSlot2 / LINE) % 2 == 1,
    GOFormat("bad regrown block slot size {}", blockSlot2));
  for (const GOSoundSampler *s : drawn3)
    GOAssert(
      s->stream.GetBlockData() != nullptr
        && (uintptr_t)s->stream.GetBlockData() % LINE == 0,
      "voice block not reattached after stride change");

  // Stride 0 detaches (subblock path off).
  pool.ReturnAll();
  pool.SetVoiceBlockChannelStride(0);

  std::vector<GOSoundSampler *> drawn4 = DrainPool(pool);

  for (const GOSoundSampler *s : drawn4)
    GOAssert(
      s->stream.GetBlockData() == nullptr, "voice block survived stride 0");
}

void GOTestSamplerPool::run() { TestArenaGeometry(); }
