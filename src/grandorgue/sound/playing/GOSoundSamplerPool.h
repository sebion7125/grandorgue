/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOSOUNDSAMPLERPOOL_H_
#define GOSOUNDSAMPLERPOOL_H_

#include <memory>
#include <new>
#include <vector>

#include "threading/GOMutex.h"

#include "GOSoundSimpleSamplerList.h"

struct GOSoundSampler;

class GOSoundSamplerPool {
private:
  /* The samplers and their render-block buffers live in contiguous,
   * cache-line-aligned arena chunks instead of being new'd one by one:
   * with tens of thousands of equally sized hot objects, the allocation
   * stride decides how they map onto the CPU cache sets, and the heap
   * makes that stride an uncontrolled per-process die roll (and a power
   * of two for the block buffers - the worst case). The arenas place
   * every object at a fixed line-aligned slot whose line count is
   * coprime to the set count; see OddLineSlotSize in the .cpp for the
   * arithmetic. Chunks are only ever added, never moved or freed before
   * pool destruction (except the block-buffer chunks on a stride
   * change), so sampler addresses stay stable for the engine. */
  static constexpr size_t ARENA_ALIGNMENT = 64;
  /* A chunk is either a plain aligned-new block or a 2MB-page-backed
   * block - Windows: VirtualAlloc MEM_LARGE_PAGES (with the
   * SeLockMemoryPrivilege granted); Linux: a 2MB-aligned private mmap
   * with MADV_HUGEPAGE. 2MB pages are physically contiguous, so the
   * L2/L3 cache-set mapping of the arena becomes deterministic instead
   * of drawing a new physical page assignment every process start. The
   * deleter carries which free path the chunk needs (mapBytes != 0 means
   * munmap with that length). */
  struct ArenaDeleter {
    bool largePages = false;
    size_t mapBytes = 0;
    void operator()(char *p) const noexcept;
  };
  using ArenaPtr = std::unique_ptr<char[], ArenaDeleter>;

  static bool IsLargePageArena(const ArenaPtr &arena) {
    return arena.get_deleter().largePages || arena.get_deleter().mapBytes != 0;
  }

  GOMutex m_Lock;
  std::atomic_uint m_SamplerCount;
  unsigned m_UsageLimit;
  GOSoundSimpleSamplerList m_AvailableSamplers;
  /* Non-owning register of every pooled sampler, parallel to
   * m_VoiceBlockSlots. The objects themselves live in m_SamplerArenas
   * (placement-new, trivially destructible, never individually
   * destroyed or freed). */
  std::vector<GOSoundSampler *> m_Samplers;
  std::vector<ArenaPtr> m_SamplerArenas;

  /* Per-sampler V3 render-block sample storage (see
   * GOSoundStream::m_BlockData): the pool owns these buffers because
   * GetSampler() memsets the whole GOSoundSampler on every note start -
   * an owning pointer inside the stream would leak there. Parallel to
   * m_Samplers (same indices); the slots live in m_VoiceBlockArenas.
   * Each slot holds MAX_OUTPUT_CHANNELS * m_VoiceBlockChannelStride
   * ints. */
  std::vector<int *> m_VoiceBlockSlots;
  std::vector<ArenaPtr> m_VoiceBlockArenas;
  // Ints per channel = max bin + guard; 0 = subblock path disabled.
  unsigned m_VoiceBlockChannelStride = 0;

  // Large-page bookkeeping for the diagnostics report: the last
  // VirtualAlloc/mmap error if a chunk fell back to a plain allocation
  // (1314 = privilege not held on Windows). The chunk count itself is
  // computed live from the current arenas (GetLargePageChunkCount) rather
  // than tracked here, since m_VoiceBlockArenas gets replaced wholesale
  // on a channel-stride change (SetVoiceBlockChannelStride) - a running
  // counter would keep counting chunks that were already freed.
  unsigned m_LargePageFailError = 0;

  ArenaPtr AllocArena(size_t bytes);
  void AttachVoiceBlocksFrom(unsigned firstIndex);

public:
  GOSoundSamplerPool();
  // Runs the placement-new'd samplers' destructors before the arenas go.
  ~GOSoundSamplerPool();
  GOSoundSampler *GetSampler();
  void ReturnSampler(GOSoundSampler *sampler);
  void ReturnAll();
  unsigned GetUsageLimit() const;
  void SetUsageLimit(unsigned count);
  unsigned UsedSamplerCount() const;

  /* Sets the per-channel render-block capacity (max bin + guard, in
   * ints) and (re)allocates every pooled sampler's block buffer to match.
   * Called from the engine setup, before any voice is active; 0 disables
   * the buffers (subblock path off). */
  void SetVoiceBlockChannelStride(unsigned channelStride);

  // Diagnostics (audio monitor report): arena geometry probes.
  const char *GetSamplerArenaBase() const {
    return m_SamplerArenas.empty() ? nullptr : m_SamplerArenas[0].get();
  }
  const char *GetVoiceBlockArenaBase() const {
    return m_VoiceBlockArenas.empty() ? nullptr : m_VoiceBlockArenas[0].get();
  }
  static size_t GetSamplerSlotSize();
  size_t GetVoiceBlockSlotSize() const;
  unsigned GetArenaChunkCount() const {
    return (unsigned)(m_SamplerArenas.size() + m_VoiceBlockArenas.size());
  }
  unsigned GetLargePageChunkCount() const {
    unsigned n = 0;

    for (const ArenaPtr &a : m_SamplerArenas)
      n += IsLargePageArena(a) ? 1 : 0;
    for (const ArenaPtr &a : m_VoiceBlockArenas)
      n += IsLargePageArena(a) ? 1 : 0;
    return n;
  }
  unsigned GetLargePageFailError() const { return m_LargePageFailError; }
};

inline unsigned GOSoundSamplerPool::GetUsageLimit() const {
  return m_UsageLimit;
}

inline unsigned GOSoundSamplerPool::UsedSamplerCount() const {
  return m_SamplerCount.load();
}

#endif /* GOSOUNDSAMPLERPOOL_H_ */
