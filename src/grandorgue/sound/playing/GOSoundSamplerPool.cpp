/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOSoundSamplerPool.h"

#include <assert.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#endif

#include "sound/GOSoundDefs.h"
#include "threading/GOMutexLocker.h"

#include "GOSoundSampler.h"

namespace {

constexpr size_t CACHE_LINE_SIZE = 64;

/* Rounds a payload size up to a whole number of cache lines and forces
 * the line count ODD. Rationale: an object placed at byte offset S*i
 * lands on cache set (S*i/64) mod SETS, and SETS is a power of two on
 * every relevant CPU (64 in a 32KB/8-way L1). If S/64 shares a factor
 * with SETS, tens of thousands of equally sized hot objects clump onto
 * a fraction of the sets and evict each other while most of the cache
 * idles; a 1024-byte stride puts them on 4 of 64 set positions
 * (measured 2026-07-17: audible gaps). An odd line count is coprime to
 * every power of two, so consecutive slots walk ALL sets before the
 * pattern repeats. */
constexpr size_t OddLineSlotSize(size_t payloadBytes) {
  size_t lines = (payloadBytes + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE;
  return (lines | 1) * CACHE_LINE_SIZE;
}

constexpr size_t SAMPLER_SLOT_SIZE = OddLineSlotSize(sizeof(GOSoundSampler));

static_assert(
  alignof(GOSoundSampler) <= CACHE_LINE_SIZE,
  "slot alignment is the arena alignment");
static_assert(
  (SAMPLER_SLOT_SIZE / CACHE_LINE_SIZE) % 2 == 1
    && SAMPLER_SLOT_SIZE >= sizeof(GOSoundSampler),
  "slot must cover the sampler with an odd number of cache lines");

/* SIZE GATE - the two asserts above keep the slot's SHAPE correct for any
 * sampler size; this one pins its actual VALUE, because the value is what
 * the measurements were taken at.
 *
 * The stride is what the cache sets see, so it is not a free parameter:
 * the sampler currently occupies 896 bytes (exactly 14 cache lines) and
 * the odd-line rule pads the slot to 15 lines = 960, which is what makes
 * the stride coprime to the set count.
 *
 * HEADROOM: 64 bytes, i.e. one cache line. The stride holds while the
 * sampler stays <= 960; the first byte past that takes it to 16 lines,
 * which the odd-line rule rounds to 17 -> a 1088-byte stride, +13% arena
 * footprint and a different conflict pattern for every voice at once.
 * Note the jump skips 16 lines entirely - there is no gentle degradation
 * here, it is a cliff.
 *
 * That headroom has already been spent down once: the sampler grew
 * 880 -> 896 when order 4 returned (m_hist3 went to the END of
 * GOSoundCompressionCacheV3 so orders <= 3 kept their offsets, commit
 * 1ed770a63). The stride survived, but the reserve went from 80 to 64
 * bytes. Treat the remaining line as a budget, not as slack: if this
 * assert fires, shrink or relocate the new member rather than accepting
 * the next stride. */
static_assert(
  MAX_OUTPUT_CHANNELS != 2 || SAMPLER_SLOT_SIZE == 960,
  "sampler slot stride must stay 960 bytes (15 cache lines over an "
  "896-byte sampler); see the size-gate comment above");

char *AllocAlignedArena(size_t bytes) {
  return static_cast<char *>(
    ::operator new[](bytes, std::align_val_t{CACHE_LINE_SIZE}));
}

#ifdef _WIN32
/* SeLockMemoryPrivilege sits DISABLED in the token even when the
 * account has it assigned - it must be enabled once per process before
 * MEM_LARGE_PAGES can succeed. Returns whether it is enabled now. */
bool EnableLockMemoryPrivilege() {
  static const bool enabled = [] {
    HANDLE token = NULL;
    TOKEN_PRIVILEGES tp;
    bool ok = false;

    if (!OpenProcessToken(
          GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
      return false;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (
      LookupPrivilegeValueW(
        NULL, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid)
      && AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL)
      && GetLastError() != ERROR_NOT_ALL_ASSIGNED)
      ok = true;
    CloseHandle(token);
    return ok;
  }();
  return enabled;
}
#endif

} // namespace

/* One arena chunk. Windows first tries 2MB large pages (physically
 * contiguous -> deterministic L2/L3 set mapping, the point of the
 * exercise; also non-pageable) and falls back to a plain aligned block,
 * recording the error for the report. Large-page allocations are 2MB-
 * aligned, satisfying ARENA_ALIGNMENT by orders of magnitude. */
GOSoundSamplerPool::ArenaPtr GOSoundSamplerPool::AllocArena(size_t bytes) {
#ifdef GO_NO_LARGE_PAGES
  /* A/B build (2026-07-23): never ask for large pages at all, so the
   * arenas come back as ordinary 4KB-paged memory.
   *
   * Why this switch exists: 2MB pages are physically contiguous, which is
   * what makes their L2/L3 set mapping deterministic - the stated point of
   * the exercise above. The flip side is that the OS can no longer scatter
   * the physical frames, so every voice walking the arena at the same
   * 960-byte stride lands on the same few L3 sets by construction.
   * Measured on Rotterdam at 24k voices, this build's counterpart showed
   * an unchanged L2 miss volume (272 vs 274 M) but 9% more L3 misses
   * (90 vs 98 M) and L3HIT 0.51 -> 0.47 - i.e. the same request stream,
   * caught less often. Conflict misses are exactly that signature.
   *
   * Toggling the SeLockMemoryPrivilege instead would need a logoff cycle,
   * hence a build. The report shows the result either way: this build
   * reports "large pages 0/4" with no error code, whereas a missing
   * privilege reports error 1314. */
  m_LargePageFailError = 0;
#elif defined(_WIN32)
  const size_t largeMin = GetLargePageMinimum();

  if (largeMin > 0 && EnableLockMemoryPrivilege()) {
    const size_t rounded = (bytes + largeMin - 1) / largeMin * largeMin;
    char *p = static_cast<char *>(VirtualAlloc(
      NULL, rounded, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
      PAGE_READWRITE));

    if (p)
      return ArenaPtr(p, ArenaDeleter{true});
  }
  m_LargePageFailError = GetLastError();
#elif defined(__linux__)
  /* THP counterpart of the Windows path: a private anonymous mapping,
   * trimmed to a 2MB-aligned base (huge faults need 2MB-aligned virtual
   * chunks; mmap guarantees no such alignment), advised MADV_HUGEPAGE.
   * Counted as a large-page chunk when the advice is accepted - whether
   * pages actually materialize shows up in the report's AnonHugePages
   * line. Falls through to the aligned-new arena on any failure. */
  constexpr size_t HUGE_PAGE_SIZE = 2 * 1024 * 1024;
  const size_t rounded
    = (bytes + HUGE_PAGE_SIZE - 1) / HUGE_PAGE_SIZE * HUGE_PAGE_SIZE;
  char *raw = static_cast<char *>(mmap(
    NULL, rounded + HUGE_PAGE_SIZE, PROT_READ | PROT_WRITE,
    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));

  if (raw != MAP_FAILED) {
    const size_t misalign = (uintptr_t)raw % HUGE_PAGE_SIZE;
    const size_t headTrim = misalign ? HUGE_PAGE_SIZE - misalign : 0;
    char *p = raw + headTrim;

    if (headTrim)
      munmap(raw, headTrim);
    if (headTrim < HUGE_PAGE_SIZE)
      munmap(p + rounded, HUGE_PAGE_SIZE - headTrim);
    if (madvise(p, rounded, MADV_HUGEPAGE) == 0)
      return ArenaPtr(p, ArenaDeleter{false, rounded});
    m_LargePageFailError = (unsigned)errno;
    munmap(p, rounded);
  } else
    m_LargePageFailError = (unsigned)errno;
#endif
  return ArenaPtr(AllocAlignedArena(bytes), ArenaDeleter{false});
}

void GOSoundSamplerPool::ArenaDeleter::operator()(char *p) const noexcept {
#ifdef _WIN32
  if (largePages) {
    VirtualFree(p, 0, MEM_RELEASE);
    return;
  }
#elif defined(__linux__)
  if (mapBytes) {
    munmap(p, mapBytes);
    return;
  }
#endif
  ::operator delete[](p, std::align_val_t{ARENA_ALIGNMENT});
}

size_t GOSoundSamplerPool::GetSamplerSlotSize() { return SAMPLER_SLOT_SIZE; }

size_t GOSoundSamplerPool::GetVoiceBlockSlotSize() const {
  return m_VoiceBlockChannelStride > 0
    ? OddLineSlotSize(
        sizeof(int) * MAX_OUTPUT_CHANNELS * m_VoiceBlockChannelStride)
    : 0;
}

GOSoundSamplerPool::GOSoundSamplerPool()
  : m_SamplerCount(0), m_UsageLimit(0), m_AvailableSamplers(), m_Samplers() {
  ReturnAll();
}

GOSoundSamplerPool::~GOSoundSamplerPool() {
  /* The samplers are placement-new'd into the arenas, so their
   * destructors must run manually before the arena chunks are released
   * (member destruction order). Not a formality: GOSoundFader holds
   * shared_ptrs to crossfade templates, so ~GOSoundSampler is
   * non-trivial. */
  for (unsigned i = 0; i < m_Samplers.size(); i++)
    m_Samplers[i]->~GOSoundSampler();
}

void GOSoundSamplerPool::ReturnAll() {
  GOMutexLocker locker(m_Lock);

  m_SamplerCount = 0;
  m_AvailableSamplers.Clear();

  /* The pool never shrinks: the samplers live in arena chunks
   * (high-water-mark memory, ~1KB per sampler). A lowered usage limit
   * is enforced by GetSampler's count gate; freelist entries beyond the
   * limit are simply never handed out. */
  for (unsigned i = 0; i < m_Samplers.size(); i++)
    m_AvailableSamplers.Put(m_Samplers[i]);
}

/* Allocates one contiguous chunk of render-block slots for the samplers
 * [firstIndex, m_Samplers.size()) and attaches them. Caller holds
 * m_Lock. The payload is naturally a power of two (channels * stride *
 * sizeof(int)) - the worst possible spacing (see OddLineSlotSize) - so
 * the slot rounds up to the next odd line count. */
void GOSoundSamplerPool::AttachVoiceBlocksFrom(unsigned firstIndex) {
  const unsigned count = m_Samplers.size() - firstIndex;

  if (m_VoiceBlockChannelStride > 0 && count > 0) {
    const size_t slotSize = GetVoiceBlockSlotSize();

    m_VoiceBlockArenas.push_back(AllocArena((size_t)count * slotSize));

    char *arena = m_VoiceBlockArenas.back().get();

    for (unsigned i = 0; i < count; i++)
      m_VoiceBlockSlots[firstIndex + i]
        = reinterpret_cast<int *>(arena + (size_t)i * slotSize);
  } else {
    for (unsigned i = firstIndex; i < m_Samplers.size(); i++)
      m_VoiceBlockSlots[i] = nullptr;
  }
  for (unsigned i = firstIndex; i < m_Samplers.size(); i++)
    m_Samplers[i]->stream.SetBlockData(
      m_VoiceBlockSlots[i], m_VoiceBlockChannelStride);
}

void GOSoundSamplerPool::SetVoiceBlockChannelStride(unsigned channelStride) {
  GOMutexLocker locker(m_Lock);

  if (channelStride == m_VoiceBlockChannelStride)
    return;
  m_VoiceBlockChannelStride = channelStride;

  /* Build the replacement chunk and attach every sampler before the old
   * chunks are released (end of scope) - no window with dangling block
   * pointers. Only called from engine setup, no voices active. */
  std::vector<ArenaPtr> oldArenas;
  oldArenas.swap(m_VoiceBlockArenas);
  AttachVoiceBlocksFrom(0);
}

void GOSoundSamplerPool::SetUsageLimit(unsigned count) {
  m_UsageLimit = count;

  GOMutexLocker locker(m_Lock);
  if (m_Samplers.size() >= m_UsageLimit)
    return;

  const unsigned firstNew = m_Samplers.size();
  const unsigned newCount = m_UsageLimit - firstNew;

  m_SamplerArenas.push_back(
    AllocArena((size_t)newCount * SAMPLER_SLOT_SIZE));

  char *arena = m_SamplerArenas.back().get();

  for (unsigned i = 0; i < newCount; i++) {
    GOSoundSampler *sampler
      = new (arena + (size_t)i * SAMPLER_SLOT_SIZE) GOSoundSampler();

    m_Samplers.push_back(sampler);
    m_VoiceBlockSlots.push_back(nullptr);
  }
  AttachVoiceBlocksFrom(firstNew);
  /* Publish to the lock-free freelist only AFTER the block buffers are
   * attached - GetSampler takes no lock and may grab a sampler the
   * moment it is Put. */
  for (unsigned i = firstNew; i < m_Samplers.size(); i++) {
    m_SamplerCount.fetch_add(1);
    ReturnSampler(m_Samplers[i]);
  }
}

GOSoundSampler *GOSoundSamplerPool::GetSampler() {
  GOSoundSampler *sampler = NULL;

  if (m_SamplerCount < m_UsageLimit) {
    sampler = m_AvailableSamplers.Get();
    if (sampler)
      m_SamplerCount.fetch_add(1);
  }
  if (sampler) {
    /* The stream's block-storage pointer lives inside the memset range;
     * the buffer is pool-owned and tied to this sampler slot, so restore
     * it right after clearing (see GOSoundStream::m_BlockData). */
    int *pBlockData = sampler->stream.GetBlockData();

    memset((void *)sampler, 0, sizeof(GOSoundSampler));
    sampler->stream.SetBlockData(pBlockData, m_VoiceBlockChannelStride);
  }
  return sampler;
}

void GOSoundSamplerPool::ReturnSampler(GOSoundSampler *sampler) {
  assert(m_SamplerCount > 0);
  m_SamplerCount.fetch_add(-1);
  m_AvailableSamplers.Put(sampler);
}
