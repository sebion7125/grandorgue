// v3_subblock_cache_check.cpp
//
// Synthetic decoded render-subblock cache prototype. This targets the
// scheduling/cache-miss risk before any production hotpath or serialization
// code is touched.
//
// It checks three policies:
//   1. blocking miss resolution;
//   2. duplicate build allowed while another worker is building;
//   3. try-lock failure counted as synchronous fallback.
//
// Build:
//   g++ -O3 -std=gnu++17 -pthread v3_subblock_cache_check.cpp \
//     -o v3_subblock_cache_check
//
// Usage:
//   v3_subblock_cache_check

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr unsigned CHANNELS = 2;
constexpr unsigned GUARD_SAMPLES = 8;
constexpr unsigned SUBBLOCK_SAMPLES = 128;
constexpr unsigned WORKERS = 8;
constexpr unsigned REQUESTS_PER_WORKER = 96;
constexpr unsigned MAX_DUPLICATE_BUILDS_PER_ENTRY = 2;

enum class Policy {
  Blocking,
  DuplicateBuildAllowed,
  TryLockFallback,
};

enum class EntryState : uint8_t {
  Building,
  Ready,
  Failed,
};

enum class SegmentKind : uint8_t {
  MainSegment,
  EndSegment,
  FailedSegment,
};

enum class Layout : uint8_t {
  ScalarPlanar,
  BatchLaneInterleaved,
};

struct SegmentId {
  SegmentKind kind = SegmentKind::MainSegment;
  unsigned index = 0;

  bool operator<(const SegmentId &o) const {
    if (kind != o.kind)
      return kind < o.kind;
    return index < o.index;
  }

  bool operator==(const SegmentId &o) const {
    return kind == o.kind && index == o.index;
  }
};

struct Key {
  SegmentId segment;
  Layout layout = Layout::ScalarPlanar;
  unsigned start = 0;
  unsigned renderSamples = 0;
  unsigned guardSamples = 0;

  bool operator<(const Key &o) const {
    if (!(segment == o.segment))
      return segment < o.segment;
    if (layout != o.layout)
      return layout < o.layout;
    if (start != o.start)
      return start < o.start;
    if (renderSamples != o.renderSamples)
      return renderSamples < o.renderSamples;
    return guardSamples < o.guardSamples;
  }
};

unsigned samplesIncludingGuard(const Key &key) {
  return key.renderSamples + key.guardSamples;
}

struct DecodedSubblock {
  Key key;
  std::vector<int32_t> decoded; // planar
  uint64_t checksum = 0;
};

struct Entry {
  explicit Entry(const Key &k) : key(k) {}

  Key key;
  EntryState state = EntryState::Building;
  std::vector<int32_t> decoded;
  uint64_t checksum = 0;
  unsigned duplicateBuildClaims = 0;
  std::mutex mutex;
  std::condition_variable cv;
};

struct Counters {
  std::atomic<unsigned> requests{0};
  std::atomic<unsigned> hits{0};
  std::atomic<unsigned> misses{0};
  std::atomic<unsigned> builds{0};
  std::atomic<unsigned> duplicateBuilds{0};
  std::atomic<unsigned> duplicateWaits{0};
  std::atomic<unsigned> syncFallbacks{0};
  std::atomic<unsigned> waits{0};
  std::atomic<unsigned> immutableChecks{0};
  std::atomic<unsigned> readyOverwriteAttempts{0};
  std::atomic<unsigned> failedResults{0};
  std::atomic<unsigned> largeBuildsUnderMapLock{0};
};

bool isBuildFailureKey(const Key &key) {
  return key.segment.kind == SegmentKind::FailedSegment;
}

uint32_t segmentSeed(const SegmentId &segment) {
  uint32_t x = (segment.index + 1) * 0xc2b2ae35u;
  x ^= (uint32_t)segment.kind * 0x27d4eb2du;
  return x;
}

int32_t sampleAt(const SegmentId &segment, unsigned pos, unsigned ch) {
  uint32_t x = 0x85ebca6bu;
  x ^= segmentSeed(segment);
  x ^= pos * 1103515245u;
  x ^= (ch + 1) * 2654435761u;
  return (int32_t)((x & 0x3ffffu) - 0x20000);
}

uint64_t checksum(const std::vector<int32_t> &v) {
  uint64_t h = 1469598103934665603ull;
  for (int32_t x : v) {
    h ^= (uint32_t)x;
    h *= 1099511628211ull;
  }
  return h;
}

DecodedSubblock buildSubblock(
  const Key &key, Counters &counters, bool mapLockHeld) {
  if (mapLockHeld)
    counters.largeBuildsUnderMapLock++;

  DecodedSubblock out;
  out.key = key;
  if (isBuildFailureKey(key))
    return out;

  const unsigned samples = samplesIncludingGuard(key);
  out.decoded.resize((size_t)CHANNELS * samples);
  for (unsigned ch = 0; ch < CHANNELS; ch++)
    for (unsigned i = 0; i < samples; i++)
      out.decoded[(size_t)ch * samples + i]
        = sampleAt(key.segment, key.start + i, ch);

  // Give worker races a deterministic window without making the test slow.
  std::this_thread::sleep_for(std::chrono::microseconds(50));
  out.checksum = checksum(out.decoded);
  return out;
}

bool verifySubblock(const DecodedSubblock &b, const char *policyName) {
  if (checksum(b.decoded) != b.checksum) {
    printf("%s: checksum mismatch for start=%u\n", policyName, b.key.start);
    return false;
  }
  const unsigned samples = samplesIncludingGuard(b.key);
  for (unsigned ch = 0; ch < CHANNELS; ch++) {
    for (unsigned i = 0; i < samples; i++) {
      const int32_t want = sampleAt(b.key.segment, b.key.start + i, ch);
      const int32_t got = b.decoded[(size_t)ch * samples + i];
      if (got != want) {
        printf(
          "%s: sample mismatch start=%u pos=%u ch=%u got=%d want=%d\n",
          policyName, b.key.start, b.key.start + i, ch, got, want);
        return false;
      }
    }
  }
  return true;
}

class Cache {
public:
  Cache(Policy policy, Counters &counters)
    : m_policy(policy), m_counters(counters) {}

  DecodedSubblock request(const Key &key) {
    m_counters.requests++;
    switch (m_policy) {
    case Policy::Blocking:
      return requestBlocking(key);
    case Policy::DuplicateBuildAllowed:
      return requestDuplicateAllowed(key);
    case Policy::TryLockFallback:
      return requestTryLockFallback(key);
    }
    return buildOutsideMapLock(key);
  }

  size_t entryCount() {
    std::lock_guard<std::mutex> mapLock(m_mapMutex);
    return m_entries.size();
  }

  EntryState stateFor(const Key &key) {
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard<std::mutex> mapLock(m_mapMutex);
      auto it = m_entries.find(key);
      if (it == m_entries.end())
        return EntryState::Failed;
      entry = it->second;
    }
    std::lock_guard<std::mutex> lock(entry->mutex);
    return entry->state;
  }

  bool debugTryOverwriteReady(const Key &key) {
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard<std::mutex> mapLock(m_mapMutex);
      auto it = m_entries.find(key);
      if (it == m_entries.end())
        return false;
      entry = it->second;
    }

    DecodedSubblock altered = buildOutsideMapLock(key);
    if (!altered.decoded.empty())
      altered.decoded[0] ^= 1;
    altered.checksum = checksum(altered.decoded);
    return publish(entry, std::move(altered));
  }

private:
  Policy m_policy;
  Counters &m_counters;
  std::map<Key, std::shared_ptr<Entry>> m_entries;
  std::mutex m_mapMutex;

  DecodedSubblock buildOutsideMapLock(const Key &key) {
    return buildSubblock(key, m_counters, false);
  }

  DecodedSubblock makeResult(const std::shared_ptr<Entry> &entry) {
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (entry->state == EntryState::Failed) {
      m_counters.failedResults++;
      return {entry->key, {}, 0};
    }
    if (entry->state != EntryState::Ready) {
      m_counters.failedResults++;
      return {entry->key, {}, 0};
    }
    m_counters.immutableChecks++;
    if (checksum(entry->decoded) != entry->checksum) {
      m_counters.failedResults++;
      return {entry->key, {}, 0};
    }
    return {entry->key, entry->decoded, entry->checksum};
  }

  bool publish(const std::shared_ptr<Entry> &entry, DecodedSubblock &&built) {
    {
      std::lock_guard<std::mutex> lock(entry->mutex);
      if (entry->state == EntryState::Ready) {
        m_counters.readyOverwriteAttempts++;
        return false;
      }
      if (entry->state == EntryState::Failed)
        return false;
      if (built.decoded.empty()) {
        entry->decoded.clear();
        entry->checksum = 0;
        entry->state = EntryState::Failed;
        entry->cv.notify_all();
        return false;
      }
      entry->decoded = std::move(built.decoded);
      entry->checksum = checksum(entry->decoded);
      entry->state = EntryState::Ready;
    }
    entry->cv.notify_all();
    return true;
  }

  DecodedSubblock requestBlocking(const Key &key) {
    std::shared_ptr<Entry> entry;
    bool shouldBuild = false;
    {
      std::lock_guard<std::mutex> mapLock(m_mapMutex);
      auto it = m_entries.find(key);
      if (it == m_entries.end()) {
        entry = std::make_shared<Entry>(key);
        m_entries[key] = entry;
        shouldBuild = true;
        m_counters.misses++;
      } else {
        entry = it->second;
      }
    }

    if (shouldBuild) {
      m_counters.builds++;
      publish(entry, buildOutsideMapLock(key));
      return makeResult(entry);
    }

    std::unique_lock<std::mutex> lock(entry->mutex);
    if (entry->state == EntryState::Ready) {
      m_counters.hits++;
      lock.unlock();
      return makeResult(entry);
    }
    m_counters.waits++;
    entry->cv.wait(lock, [&] { return entry->state != EntryState::Building; });
    lock.unlock();
    return makeResult(entry);
  }

  DecodedSubblock requestDuplicateAllowed(const Key &key) {
    std::shared_ptr<Entry> entry;
    bool shouldBuildCanonical = false;
    bool shouldBuildDuplicate = false;
    bool shouldReturnReady = false;
    bool shouldReturnFailed = false;
    {
      std::lock_guard<std::mutex> mapLock(m_mapMutex);
      auto it = m_entries.find(key);
      if (it == m_entries.end()) {
        entry = std::make_shared<Entry>(key);
        m_entries[key] = entry;
        shouldBuildCanonical = true;
        m_counters.misses++;
      } else {
        entry = it->second;
        std::lock_guard<std::mutex> entryLock(entry->mutex);
        if (entry->state == EntryState::Ready) {
          m_counters.hits++;
          shouldReturnReady = true;
        }
        else if (entry->state == EntryState::Failed) {
          shouldReturnFailed = true;
        }
        else if (entry->duplicateBuildClaims < MAX_DUPLICATE_BUILDS_PER_ENTRY) {
          entry->duplicateBuildClaims++;
          shouldBuildDuplicate = true;
          m_counters.duplicateBuilds++;
        } else {
          m_counters.duplicateWaits++;
        }
      }
    }

    if (shouldReturnReady)
      return makeResult(entry);
    if (shouldReturnFailed)
      return makeResult(entry);

    if (shouldBuildCanonical) {
      m_counters.builds++;
      publish(entry, buildOutsideMapLock(key));
      return makeResult(entry);
    }

    if (shouldBuildDuplicate) {
      m_counters.builds++;
      return buildOutsideMapLock(key);
    }

    std::unique_lock<std::mutex> lock(entry->mutex);
    m_counters.waits++;
    entry->cv.wait(lock, [&] { return entry->state != EntryState::Building; });
    lock.unlock();
    return makeResult(entry);
  }

  DecodedSubblock requestTryLockFallback(const Key &key) {
    if (!m_mapMutex.try_lock()) {
      m_counters.syncFallbacks++;
      m_counters.builds++;
      return buildOutsideMapLock(key);
    }

    std::shared_ptr<Entry> entry;
    bool shouldBuild = false;
    auto it = m_entries.find(key);
    if (it == m_entries.end()) {
      entry = std::make_shared<Entry>(key);
      m_entries[key] = entry;
      shouldBuild = true;
      m_counters.misses++;
    } else {
      entry = it->second;
    }
    m_mapMutex.unlock();

    if (shouldBuild) {
      m_counters.builds++;
      publish(entry, buildOutsideMapLock(key));
      return makeResult(entry);
    }

    std::unique_lock<std::mutex> lock(entry->mutex);
    if (entry->state == EntryState::Ready) {
      m_counters.hits++;
      lock.unlock();
      return makeResult(entry);
    }
    m_counters.syncFallbacks++;
    lock.unlock();
    m_counters.builds++;
    return buildOutsideMapLock(key);
  }
};

Key makeKey(unsigned worker, unsigned requestIndex) {
  // Deliberately many collisions: all workers walk the same small key space
  // with phase offsets, forcing hits, waits, and duplicate-build races.
  const unsigned logical = (requestIndex + worker * 3) % 24;
  return {
    {
      logical % 4 == 0 ? SegmentKind::EndSegment : SegmentKind::MainSegment,
      logical % 3,
    },
    logical % 2 == 0 ? Layout::ScalarPlanar : Layout::BatchLaneInterleaved,
    (logical % 8) * SUBBLOCK_SAMPLES,
    SUBBLOCK_SAMPLES,
    GUARD_SAMPLES + (logical % 3),
  };
}

const char *policyName(Policy policy) {
  switch (policy) {
  case Policy::Blocking:
    return "blocking";
  case Policy::DuplicateBuildAllowed:
    return "duplicate-build";
  case Policy::TryLockFallback:
    return "trylock-fallback";
  }
  return "unknown";
}

bool runPolicy(Policy policy) {
  Counters counters;
  Cache cache(policy, counters);
  std::atomic<bool> failed{false};
  std::atomic<unsigned> ready{0};

  auto worker = [&](unsigned workerId) {
    ready++;
    while (ready.load() < WORKERS)
      std::this_thread::yield();

    for (unsigned i = 0; i < REQUESTS_PER_WORKER; i++) {
      DecodedSubblock b = cache.request(makeKey(workerId, i));
      if (!verifySubblock(b, policyName(policy)))
        failed = true;
    }
  };

  const auto t0 = Clock::now();
  std::vector<std::thread> threads;
  for (unsigned w = 0; w < WORKERS; w++)
    threads.emplace_back(worker, w);
  for (auto &t : threads)
    t.join();
  const auto t1 = Clock::now();

  if (failed)
    return false;

  const unsigned requests = counters.requests.load();
  const unsigned hits = counters.hits.load();
  const unsigned misses = counters.misses.load();
  const unsigned builds = counters.builds.load();
  const unsigned duplicates = counters.duplicateBuilds.load();
  const unsigned duplicateWaits = counters.duplicateWaits.load();
  const unsigned fallbacks = counters.syncFallbacks.load();
  const unsigned waits = counters.waits.load();
  const unsigned immutableChecks = counters.immutableChecks.load();
  const unsigned readyOverwriteAttempts = counters.readyOverwriteAttempts.load();
  const unsigned failedResults = counters.failedResults.load();
  const unsigned largeBuildsUnderMapLock = counters.largeBuildsUnderMapLock.load();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

  if (requests != WORKERS * REQUESTS_PER_WORKER) {
    printf("%s: request counter mismatch got=%u\n", policyName(policy), requests);
    return false;
  }
  if (misses == 0 || builds == 0) {
    printf("%s: expected misses/builds, got misses=%u builds=%u\n", policyName(policy), misses, builds);
    return false;
  }
  if (policy == Policy::Blocking && waits == 0) {
    printf("%s: expected at least one wait\n", policyName(policy));
    return false;
  }
  if (policy == Policy::DuplicateBuildAllowed && duplicates == 0) {
    printf("%s: expected duplicate builds\n", policyName(policy));
    return false;
  }
  if (duplicates > misses * MAX_DUPLICATE_BUILDS_PER_ENTRY) {
    printf(
      "%s: duplicate builds unbounded duplicates=%u misses=%u max-per-entry=%u\n",
      policyName(policy),
      duplicates,
      misses,
      MAX_DUPLICATE_BUILDS_PER_ENTRY);
    return false;
  }
  if (policy == Policy::TryLockFallback && fallbacks == 0) {
    printf("%s: expected sync fallbacks\n", policyName(policy));
    return false;
  }
  if (policy != Policy::TryLockFallback && fallbacks != 0) {
    printf("%s: unexpected sync fallbacks=%u\n", policyName(policy), fallbacks);
    return false;
  }
  if (immutableChecks == 0) {
    printf("%s: no immutable ready-entry checks happened\n", policyName(policy));
    return false;
  }
  if (readyOverwriteAttempts != 0) {
    printf("%s: ready entry was overwritten attempts=%u\n", policyName(policy), readyOverwriteAttempts);
    return false;
  }
  if (failedResults != 0) {
    printf("%s: unexpected failed results=%u\n", policyName(policy), failedResults);
    return false;
  }
  if (largeBuildsUnderMapLock != 0) {
    printf("%s: large build happened under map lock=%u\n", policyName(policy), largeBuildsUnderMapLock);
    return false;
  }
  if (cache.entryCount() != 24) {
    printf("%s: cache key collapse, entries=%zu expected=24\n", policyName(policy), cache.entryCount());
    return false;
  }

  printf(
    "%s: OK requests=%u hits=%u misses=%u builds=%u duplicateBuilds=%u "
    "duplicateWaits=%u syncFallbacks=%u waits=%u immutableChecks=%u "
    "largeBuildsUnderMapLock=%u elapsed=%.3fms\n",
    policyName(policy),
    requests,
    hits,
    misses,
    builds,
    duplicates,
    duplicateWaits,
    fallbacks,
    waits,
    immutableChecks,
    largeBuildsUnderMapLock,
    ms);
  return true;
}

bool runKeyContractCheck() {
  Counters counters;
  Cache cache(Policy::Blocking, counters);
  const Key base{
    {SegmentKind::MainSegment, 3},
    Layout::ScalarPlanar,
    SUBBLOCK_SAMPLES,
    SUBBLOCK_SAMPLES,
    GUARD_SAMPLES,
  };
  const Key samePositionDifferentLayout{
    base.segment,
    Layout::BatchLaneInterleaved,
    base.start,
    base.renderSamples,
    base.guardSamples,
  };
  const Key samePositionDifferentGuard{
    base.segment,
    base.layout,
    base.start,
    base.renderSamples,
    base.guardSamples + 3,
  };
  const Key sameVirtualPositionDifferentSegment{
    {SegmentKind::EndSegment, base.segment.index},
    base.layout,
    base.start,
    base.renderSamples,
    base.guardSamples,
  };

  const Key keys[] = {
    base,
    samePositionDifferentLayout,
    samePositionDifferentGuard,
    sameVirtualPositionDifferentSegment,
  };

  for (const Key &key : keys) {
    DecodedSubblock b = cache.request(key);
    if (!verifySubblock(b, "key-contract"))
      return false;
  }

  if (cache.entryCount() != 4) {
    printf("key-contract: key fields collapsed entries=%zu expected=4\n", cache.entryCount());
    return false;
  }

  if (cache.debugTryOverwriteReady(base)) {
    printf("key-contract: Ready entry overwrite was accepted\n");
    return false;
  }
  if (counters.readyOverwriteAttempts.load() != 1) {
    printf(
      "key-contract: overwrite attempt was not instrumented got=%u\n",
      counters.readyOverwriteAttempts.load());
    return false;
  }

  DecodedSubblock afterOverwriteAttempt = cache.request(base);
  if (!verifySubblock(afterOverwriteAttempt, "key-contract"))
    return false;

  printf(
    "key-contract: OK entries=%zu readyOverwriteAttempts=%u immutableChecks=%u\n",
    cache.entryCount(),
    counters.readyOverwriteAttempts.load(),
    counters.immutableChecks.load());
  return true;
}

bool runFailedStateCheck() {
  Counters counters;
  Cache cache(Policy::Blocking, counters);
  const Key failedKey{
    {SegmentKind::FailedSegment, 1},
    Layout::ScalarPlanar,
    0,
    SUBBLOCK_SAMPLES,
    GUARD_SAMPLES,
  };

  DecodedSubblock first = cache.request(failedKey);
  DecodedSubblock second = cache.request(failedKey);
  if (!first.decoded.empty() || !second.decoded.empty()) {
    printf("failed-state: failed entry returned decoded samples\n");
    return false;
  }
  if (cache.stateFor(failedKey) != EntryState::Failed) {
    printf("failed-state: entry did not remain Failed\n");
    return false;
  }
  if (counters.builds.load() != 1) {
    printf("failed-state: failed entry was rebuilt builds=%u\n", counters.builds.load());
    return false;
  }
  if (counters.failedResults.load() != 2) {
    printf("failed-state: failed results not instrumented got=%u\n", counters.failedResults.load());
    return false;
  }

  printf(
    "failed-state: OK builds=%u failedResults=%u\n",
    counters.builds.load(),
    counters.failedResults.load());
  return true;
}

} // namespace

int main() {
  const Policy policies[] = {
    Policy::Blocking,
    Policy::DuplicateBuildAllowed,
    Policy::TryLockFallback,
  };

  int checked = 0;
  int failures = 0;
  for (Policy policy : policies) {
    checked++;
    if (!runPolicy(policy))
      failures++;
  }
  checked++;
  if (!runKeyContractCheck())
    failures++;
  checked++;
  if (!runFailedStateCheck())
    failures++;

  printf("\n%d/%d cache policy checks passed\n", checked - failures, checked);
  return failures ? 1 : 0;
}
