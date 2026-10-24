// v3_segment_subblock_check.cpp
//
// Synthetic harness for Phase 2.5b. It models GO-style MainSegment /
// EndSegment / StartSegment playback and checks that decoded render subblocks
// can represent loop boundaries, jump overflow, and polyphase guard samples
// before any production hotpath code is touched.
//
// This intentionally does not test compression speed. It tests segment
// semantics and subblock window correctness.
//
// Build:
//   g++ -O3 -std=gnu++17 v3_segment_subblock_check.cpp \
//     -o v3_segment_subblock_check
//
// Usage:
//   v3_segment_subblock_check

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr unsigned GUARD_SAMPLES = 8;
constexpr unsigned SUBBLOCK_SIZES[] = {32, 64, 128};
constexpr float FIR[GUARD_SAMPLES]
  = {0.03125f, -0.0625f, 0.125f, 0.375f, 0.375f, 0.125f, -0.0625f, 0.03125f};

enum class SegmentKind {
  MainSegment,
  EndSegment,
};

struct SegmentId {
  SegmentKind kind;
  unsigned index;
};

struct StartSegment {
  unsigned startOffset;
};

struct EndSegment {
  unsigned transitionPosition;
  unsigned endPos;
  int nextStartSegmentIndex;
  std::vector<int32_t> endData; // planar: channel * endLength + sample
};

struct Fixture {
  std::string name;
  unsigned channels = 0;
  unsigned mainSamples = 0;
  std::vector<int32_t> mainData; // planar
  std::vector<StartSegment> starts;
  std::vector<EndSegment> ends;
  std::vector<unsigned> endForStart;
  unsigned initialStartSegment = 0;
  unsigned sourceIncrement = 1;
  unsigned outputFrames = 0;
  bool requireSameVirtualEndDistinguish = false;
};

struct PlaybackState {
  unsigned currentStartSegment = 0;
  unsigned currentEndSegment = 0;
  unsigned position = 0;
  unsigned loopJumps = 0;
  unsigned overflowJumps = 0;
  unsigned terminalEndHits = 0;
};

struct Subblock {
  SegmentId segment;
  unsigned start = 0;
  unsigned samples = 0;
  std::vector<int32_t> decoded; // planar
};

int32_t formulaSample(unsigned pos, unsigned ch) {
  uint32_t x = 0x9e3779b9u;
  x ^= pos * 1103515245u;
  x ^= (ch + 1) * 2654435761u;
  x ^= (pos >> 3) * 2246822519u;
  return (int32_t)((x & 0x1ffffu) - 0x10000);
}

int32_t mainAt(const Fixture &f, unsigned pos, unsigned ch) {
  if (pos >= f.mainSamples)
    return 0;
  return f.mainData[(size_t)ch * f.mainSamples + pos];
}

unsigned endLength(const EndSegment &e) {
  return e.endPos - e.transitionPosition + GUARD_SAMPLES * 2;
}

int32_t endAt(const Fixture &f, const EndSegment &e, unsigned pos, unsigned ch) {
  if (pos < e.transitionPosition)
    return mainAt(f, pos, ch);
  const unsigned off = pos - e.transitionPosition;
  const unsigned len = endLength(e);
  if (off >= len)
    return 0;
  return e.endData[(size_t)ch * len + off];
}

int32_t segmentAt(
  const Fixture &f, const SegmentId &segment, unsigned pos, unsigned ch) {
  if (segment.kind == SegmentKind::MainSegment)
    return mainAt(f, pos, ch);
  return endAt(f, f.ends[segment.index], pos, ch);
}

SegmentId activeSegment(const Fixture &f, const PlaybackState &st) {
  const EndSegment &e = f.ends[st.currentEndSegment];
  if (st.position < e.transitionPosition)
    return {SegmentKind::MainSegment, 0};
  return {SegmentKind::EndSegment, st.currentEndSegment};
}

int32_t referenceSample(const Fixture &f, const PlaybackState &st, unsigned ch) {
  return segmentAt(f, activeSegment(f, st), st.position, ch);
}

float referenceFir(const Fixture &f, const PlaybackState &st, unsigned ch) {
  const SegmentId segment = activeSegment(f, st);
  float sum = 0.0f;
  for (unsigned i = 0; i < GUARD_SAMPLES; i++)
    sum += segmentAt(f, segment, st.position + i, ch) * FIR[i];
  return sum;
}

void advance(Fixture &f, PlaybackState &st) {
  st.position += f.sourceIncrement;
  const EndSegment &e = f.ends[st.currentEndSegment];
  if (st.position >= e.endPos) {
    const unsigned overflow = st.position - e.endPos;
    const int next = e.nextStartSegmentIndex;
    if (next < 0) {
      st.terminalEndHits++;
      st.position = e.endPos + overflow;
      return;
    }
    if (overflow > 0)
      st.overflowJumps++;
    st.loopJumps++;
    st.currentStartSegment = (unsigned)next;
    st.currentEndSegment = f.endForStart[st.currentStartSegment];
    st.position = f.starts[st.currentStartSegment].startOffset + overflow;
  }
}

Subblock buildSubblock(
  const Fixture &f, const SegmentId &segment, unsigned start, unsigned subblockSize) {
  Subblock b;
  b.segment = segment;
  b.start = start;
  b.samples = subblockSize + GUARD_SAMPLES;
  b.decoded.resize((size_t)f.channels * b.samples);
  for (unsigned ch = 0; ch < f.channels; ch++)
    for (unsigned i = 0; i < b.samples; i++)
      b.decoded[(size_t)ch * b.samples + i]
        = segmentAt(f, segment, start + i, ch);
  return b;
}

int32_t subblockSample(const Subblock &b, unsigned channels, unsigned pos, unsigned ch) {
  (void)channels;
  return b.decoded[(size_t)ch * b.samples + (pos - b.start)];
}

float subblockFir(const Subblock &b, unsigned pos, unsigned ch) {
  float sum = 0.0f;
  const unsigned offset = pos - b.start;
  for (unsigned i = 0; i < GUARD_SAMPLES; i++)
    sum += b.decoded[(size_t)ch * b.samples + offset + i] * FIR[i];
  return sum;
}

bool sameSegment(const SegmentId &a, const SegmentId &b) {
  return a.kind == b.kind && a.index == b.index;
}

const char *segmentName(SegmentKind kind) {
  return kind == SegmentKind::MainSegment ? "MainSegment" : "EndSegment";
}

bool fail(
  const Fixture &f,
  unsigned subblockSize,
  unsigned frame,
  const PlaybackState &st,
  const SegmentId &seg,
  unsigned ch,
  const char *what,
  double got,
  double want) {
  printf(
    "%s: %s mismatch subblock=%u frame=%u pos=%u segment=%s[%u] ch=%u "
    "got=%.6f want=%.6f\n",
    f.name.c_str(),
    what,
    subblockSize,
    frame,
    st.position,
    segmentName(seg.kind),
    seg.index,
    ch,
    got,
    want);
  return false;
}

bool runFixture(const Fixture &fixture, unsigned subblockSize) {
  Fixture f = fixture;
  PlaybackState st;
  st.currentStartSegment = f.initialStartSegment;
  st.currentEndSegment = f.endForStart[st.currentStartSegment];
  st.position = f.starts[st.currentStartSegment].startOffset;

  unsigned currentRequests = 0;
  unsigned nextRequests = 0;
  unsigned jumpTargetRequests = 0;
  unsigned endSegmentReads = 0;
  unsigned endSegmentIdChecks = 0;
  unsigned preparedEndDataChecks = 0;
  unsigned preparedEndDataOverrides = 0;
  unsigned sameVirtualEndDistinguishChecks = 0;
  unsigned unalignedBoundaryChecks = 0;
  unsigned zeroGuardChecks = 0;
  unsigned loopGuardChecks = 0;
  unsigned jumpTargetsPreparedBeforeJump = 0;

  for (const EndSegment &e : f.ends) {
    if (e.transitionPosition % subblockSize != 0)
      unalignedBoundaryChecks++;
    if (e.endPos % subblockSize != 0)
      unalignedBoundaryChecks++;
  }

  for (unsigned frame = 0; frame < f.outputFrames; frame++) {
    const SegmentId seg = activeSegment(f, st);
    if (seg.kind == SegmentKind::EndSegment) {
      endSegmentReads++;
      if (seg.index != st.currentEndSegment) {
        printf(
          "%s: EndSegment id mismatch, active=%u state=%u\n",
          f.name.c_str(),
          seg.index,
          st.currentEndSegment);
        return false;
      }
      endSegmentIdChecks++;
    }

    const unsigned subblockStart = (st.position / subblockSize) * subblockSize;
    const Subblock current = buildSubblock(f, seg, subblockStart, subblockSize);
    currentRequests++;

    const Subblock next = buildSubblock(
      f, seg, subblockStart + subblockSize, subblockSize);
    nextRequests++;
    if (!sameSegment(next.segment, current.segment)) {
      printf("%s: internal segment mismatch while building next subblock\n", f.name.c_str());
      return false;
    }

    const EndSegment &activeEnd = f.ends[st.currentEndSegment];
    if (
      activeEnd.nextStartSegmentIndex >= 0
      && st.position + subblockSize + GUARD_SAMPLES >= activeEnd.endPos) {
      const unsigned overflow
        = st.position + f.sourceIncrement > activeEnd.endPos
        ? st.position + f.sourceIncrement - activeEnd.endPos
        : 0;
      const unsigned jumpStart
        = f.starts[(unsigned)activeEnd.nextStartSegmentIndex].startOffset + overflow;
      const SegmentId jumpSeg = {SegmentKind::MainSegment, 0};
      const Subblock jump = buildSubblock(
        f, jumpSeg, (jumpStart / subblockSize) * subblockSize, subblockSize);
      (void)jump;
      jumpTargetRequests++;
      if (st.position < activeEnd.endPos)
        jumpTargetsPreparedBeforeJump++;
    }

    for (unsigned ch = 0; ch < f.channels; ch++) {
      const int32_t ref = referenceSample(f, st, ch);
      const int32_t got = subblockSample(current, f.channels, st.position, ch);
      if (got != ref)
        return fail(f, subblockSize, frame, st, seg, ch, "sample", got, ref);

      const float refFir = referenceFir(f, st, ch);
      const float gotFir = subblockFir(current, st.position, ch);
      if (gotFir != refFir)
        return fail(f, subblockSize, frame, st, seg, ch, "fir", gotFir, refFir);
    }

    if (seg.kind == SegmentKind::EndSegment) {
      const EndSegment &e = f.ends[seg.index];
      const unsigned off = st.position - e.transitionPosition;
      if (off < endLength(e)) {
        for (unsigned ch = 0; ch < f.channels; ch++) {
          const int32_t prepared
            = e.endData[(size_t)ch * endLength(e) + off];
          const int32_t got = segmentAt(f, seg, st.position, ch);
          if (got != prepared)
            return fail(
              f, subblockSize, frame, st, seg, ch, "prepared-end-data",
              got, prepared);
          preparedEndDataChecks++;
          if (st.position < e.endPos && prepared != mainAt(f, st.position, ch))
            preparedEndDataOverrides++;
        }
      }

      for (unsigned other = 0; other < f.ends.size(); other++) {
        if (other == seg.index)
          continue;
        const EndSegment &sibling = f.ends[other];
        if (
          sibling.transitionPosition != e.transitionPosition
          || sibling.endPos != e.endPos)
          continue;
        for (unsigned ch = 0; ch < f.channels; ch++) {
          const int32_t here = segmentAt(f, seg, st.position, ch);
          const int32_t there
            = segmentAt(f, {SegmentKind::EndSegment, other}, st.position, ch);
          if (here != there)
            sameVirtualEndDistinguishChecks++;
        }
      }

      if (e.nextStartSegmentIndex < 0 && st.position + GUARD_SAMPLES >= e.endPos) {
        for (unsigned ch = 0; ch < f.channels; ch++) {
          for (unsigned p = e.endPos; p < st.position + GUARD_SAMPLES; p++) {
            if (segmentAt(f, seg, p, ch) != 0)
              return fail(
                f, subblockSize, frame, st, seg, ch, "zero-guard",
                segmentAt(f, seg, p, ch), 0);
            zeroGuardChecks++;
          }
        }
      } else if (
        e.nextStartSegmentIndex >= 0 && st.position + GUARD_SAMPLES >= e.endPos) {
        const unsigned loopStart
          = f.starts[(unsigned)e.nextStartSegmentIndex].startOffset;
        for (unsigned ch = 0; ch < f.channels; ch++) {
          for (unsigned p = e.endPos; p < st.position + GUARD_SAMPLES; p++) {
            const unsigned loopPos = loopStart + (p - e.endPos);
            const int32_t got = segmentAt(f, seg, p, ch);
            const int32_t want = mainAt(f, loopPos, ch);
            if (got != want)
              return fail(
                f, subblockSize, frame, st, seg, ch, "loop-guard", got, want);
            loopGuardChecks++;
          }
        }
      }
    }

    advance(f, st);
  }

  if (fixture.ends[fixture.endForStart[fixture.initialStartSegment]].nextStartSegmentIndex >= 0) {
    if (st.loopJumps == 0 || st.overflowJumps == 0 || jumpTargetRequests == 0) {
      printf(
        "%s: expected loop jumps/overflow/jump targets, got jumps=%u "
        "overflow=%u jumpTargets=%u\n",
        f.name.c_str(), st.loopJumps, st.overflowJumps, jumpTargetRequests);
      return false;
    }
    if (jumpTargetsPreparedBeforeJump == 0) {
      printf("%s: expected jumpTargetSubblock before jump\n", f.name.c_str());
      return false;
    }
  }
  if (endSegmentReads > 0) {
    if (endSegmentIdChecks == 0 || preparedEndDataChecks == 0) {
      printf(
        "%s: missing EndSegment id/prepared-data checks id=%u prepared=%u\n",
        f.name.c_str(),
        endSegmentIdChecks,
        preparedEndDataChecks);
      return false;
    }
    if (unalignedBoundaryChecks == 0) {
      printf(
        "%s: expected transition/end boundaries to be subblock-unaligned\n",
        f.name.c_str());
      return false;
    }
  }
  if (
    f.requireSameVirtualEndDistinguish
    && sameVirtualEndDistinguishChecks == 0) {
    printf(
      "%s: expected distinct EndSegment ids at the same virtual positions\n",
      f.name.c_str());
    return false;
  }

  printf(
    "%s: OK subblock=%u frames=%u channels=%u requests current=%u next=%u "
    "jumpTarget=%u jumpPre=%u endReads=%u endId=%u preparedEnd=%u "
    "preparedOverrides=%u sameVirtualEnd=%u loopGuard=%u zeroGuard=%u "
    "jumps=%u overflow=%u terminalEnd=%u\n",
    f.name.c_str(),
    subblockSize,
    f.outputFrames,
    f.channels,
    currentRequests,
    nextRequests,
    jumpTargetRequests,
    jumpTargetsPreparedBeforeJump,
    endSegmentReads,
    endSegmentIdChecks,
    preparedEndDataChecks,
    preparedEndDataOverrides,
    sameVirtualEndDistinguishChecks,
    loopGuardChecks,
    zeroGuardChecks,
    st.loopJumps,
    st.overflowJumps,
    st.terminalEndHits);
  return true;
}

void fillMain(Fixture &f) {
  f.mainData.resize((size_t)f.channels * f.mainSamples);
  for (unsigned ch = 0; ch < f.channels; ch++)
    for (unsigned p = 0; p < f.mainSamples; p++)
      f.mainData[(size_t)ch * f.mainSamples + p] = formulaSample(p, ch);
}

EndSegment makeLoopEnd(
  const Fixture &f,
  unsigned transition,
  unsigned end,
  unsigned nextStartSegmentIndex,
  bool crossfadeMarker) {
  EndSegment e;
  e.transitionPosition = transition;
  e.endPos = end;
  e.nextStartSegmentIndex = (int)nextStartSegmentIndex;
  const unsigned len = endLength(e);
  e.endData.resize((size_t)f.channels * len);
  const unsigned loopStart = f.starts[nextStartSegmentIndex].startOffset;
  for (unsigned ch = 0; ch < f.channels; ch++) {
    for (unsigned i = 0; i < len; i++) {
      const unsigned virtualPos = transition + i;
      int32_t v;
      if (virtualPos < end) {
        v = mainAt(f, virtualPos, ch);
        if (crossfadeMarker && virtualPos + 12 >= end)
          v = (v + mainAt(f, loopStart + (virtualPos + 12 - end), ch)) / 2;
      } else {
        v = mainAt(f, loopStart + (virtualPos - end), ch);
      }
      e.endData[(size_t)ch * len + i] = v;
    }
  }
  return e;
}

EndSegment makeFileEnd(const Fixture &f, unsigned transition, unsigned end) {
  EndSegment e;
  e.transitionPosition = transition;
  e.endPos = end;
  e.nextStartSegmentIndex = -1;
  const unsigned len = endLength(e);
  e.endData.resize((size_t)f.channels * len);
  for (unsigned ch = 0; ch < f.channels; ch++) {
    for (unsigned i = 0; i < len; i++) {
      const unsigned virtualPos = transition + i;
      const int32_t v = virtualPos < end ? mainAt(f, virtualPos, ch) : 0;
      e.endData[(size_t)ch * len + i] = v;
    }
  }
  return e;
}

Fixture makeStereoTwoLoopFixture() {
  Fixture f;
  f.name = "stereo-two-loop-overflow";
  f.channels = 2;
  f.mainSamples = 360;
  f.starts = {{0}, {37}, {96}};
  f.initialStartSegment = 1;
  f.sourceIncrement = 17;
  f.outputFrames = 60;
  fillMain(f);
  f.ends.push_back(makeLoopEnd(f, 146, 181, 2, true));
  f.ends.push_back(makeLoopEnd(f, 229, 267, 1, false));
  f.endForStart.resize(f.starts.size());
  f.endForStart[0] = 0;
  f.endForStart[1] = 0;
  f.endForStart[2] = 1;
  return f;
}

Fixture makeSameVirtualEndSegmentFixture() {
  Fixture f;
  f.name = "same-virtual-position-endsegments";
  f.channels = 2;
  f.mainSamples = 240;
  f.starts = {{0}, {33}, {77}};
  f.initialStartSegment = 1;
  f.sourceIncrement = 9;
  f.outputFrames = 48;
  f.requireSameVirtualEndDistinguish = true;
  fillMain(f);

  // Both EndSegments occupy the same virtual source interval. They differ
  // only by SegmentId and prepared end_data content, matching GO's need to
  // keep prepared loop-end regions distinct even when their virtual positions
  // overlap.
  EndSegment end0 = makeLoopEnd(f, 121, 154, 2, false);
  EndSegment end1 = makeLoopEnd(f, 121, 154, 1, false);
  const unsigned len0 = endLength(end0);
  const unsigned len1 = endLength(end1);
  for (unsigned ch = 0; ch < f.channels; ch++) {
    for (unsigned i = 0; i < len0; i++) {
      if (end0.transitionPosition + i < end0.endPos)
        end0.endData[(size_t)ch * len0 + i] += 7000 + (int32_t)ch * 101;
    }
    for (unsigned i = 0; i < len1; i++) {
      if (end1.transitionPosition + i < end1.endPos)
        end1.endData[(size_t)ch * len1 + i] -= 9000 + (int32_t)ch * 103;
    }
  }
  f.ends.push_back(end0);
  f.ends.push_back(end1);
  f.endForStart.resize(f.starts.size());
  f.endForStart[0] = 0;
  f.endForStart[1] = 0;
  f.endForStart[2] = 1;
  return f;
}

Fixture makeMonoFileEndFixture() {
  Fixture f;
  f.name = "mono-file-end-zero-guard";
  f.channels = 1;
  f.mainSamples = 180;
  f.starts = {{0}};
  f.initialStartSegment = 0;
  f.sourceIncrement = 11;
  f.outputFrames = 20;
  fillMain(f);
  f.ends.push_back(makeFileEnd(f, 142, 171));
  f.endForStart = {0};
  return f;
}

} // namespace

int main() {
  std::vector<Fixture> fixtures;
  fixtures.push_back(makeStereoTwoLoopFixture());
  fixtures.push_back(makeSameVirtualEndSegmentFixture());
  fixtures.push_back(makeMonoFileEndFixture());

  int checked = 0;
  int failures = 0;
  for (const Fixture &f : fixtures) {
    for (unsigned subblockSize : SUBBLOCK_SIZES) {
      checked++;
      if (!runFixture(f, subblockSize))
        failures++;
    }
  }

  printf("\n%d/%d segment/subblock checks passed\n", checked - failures, checked);
  return failures ? 1 : 0;
}
