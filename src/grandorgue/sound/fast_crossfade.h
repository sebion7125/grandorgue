/*
 fast_crossfade.h
 Lightweight, header-only steppers for crossfade evaluation.
 Designed for use in the GOSoundFader hot-path: no per-sample trig/sqrt,
 no switches in the inner loop. Stepper instances are cheap (few floats)
 and can reference global LUTs prepared once.
*/

#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <memory>

 // minimal dependency to the project's enum header
#include "GOCrossfadeMode.h"

namespace GOAudioParams {
static constexpr float kFastCrossfadeHalfPi = 1.57079632679489661923f;

// --- shared sqrt LUT (used by SqrtEqualPower) ---
struct FastSqrtLUT {
  static constexpr int kSize = 1024;
  // returns pointer to table of kSize+1 floats, values sqrt(i/kSize)
  static const float* table() {
    static float tbl[kSize + 1];
    static bool init = false;
    if (!init) {
      for (int i = 0; i <= kSize; ++i)
        tbl[i] = std::sqrt(float(i) / float(kSize));
      init = true;
    }
    return tbl;
  }
};

// --- Stepper structs (fallback/utility) ---
// Each stepper exposes:
//  - void init(unsigned N, unsigned startPos = 0);
//  - inline void next(float &a, float &b); // produces a=out, b=in, advances

struct CFLinear {
  float t = 0.f;
  float dt = 0.f;
  unsigned pos = 0;
  unsigned len = 0;

  inline void init(unsigned N, unsigned startPos = 0) {
    len = (N > 0) ? N : 0;
    pos = startPos;
    dt = (len > 1) ? 1.0f / float(len - 1) : 0.0f;
    t = float(startPos) * dt;
  }

  inline void next(float &a, float &b) {
    a = 1.0f - t;
    b = t;
    t += dt;
    if (pos < len) ++pos;
  }
};

struct CFX2 {
  float t = 0.f;
  float dt = 0.f;
  unsigned pos = 0;
  unsigned len = 0;

  inline void init(unsigned N, unsigned startPos = 0) {
    len = (N > 0) ? N : 0;
    pos = startPos;
    dt = (len > 1) ? 1.0f / float(len - 1) : 0.0f;
    t = float(startPos) * dt;
  }

  inline void next(float &a, float &b) {
    const float tt = t * t;
    a = 1.0f - tt;
    b = tt;
    t += dt;
    if (pos < len) ++pos;
  }
};

struct CFSinEP {
  // recurrence values (fallback)
  float s = 0.f; // sin(theta)
  float c = 1.f; // cos(theta)
  float sd = 0.f; // sin(delta)
  float cd = 1.f; // cos(delta)
  unsigned pos = 0;
  unsigned len = 0;

  inline void init(unsigned N, unsigned startPos = 0) {
    len = (N > 0) ? N : 0;
    pos = startPos;
    const float d = (len > 1) ? (kFastCrossfadeHalfPi / float(len - 1)) : 0.f;
    sd = static_cast<float>(std::sin(d));
    cd = static_cast<float>(std::cos(d));
    // advance recurrence to startPos
    s = 0.f; c = 1.f;
    for (unsigned i = 0; i < startPos; ++i) {
      const float s0 = s, c0 = c;
      s = s0 * cd + c0 * sd;
      c = c0 * cd - s0 * sd;
    }
  }

  inline void next(float &a, float &b) {
    a = c;
    b = s;
    const float s0 = s, c0 = c;
    s = s0 * cd + c0 * sd;
    c = c0 * cd - s0 * sd;
    if (pos < len) ++pos;
  }
};

struct CFSin2 {
  // reuse sin recurrence and square outputs (fallback)
  float s = 0.f;
  float c = 1.f;
  float sd = 0.f;
  float cd = 1.f;
  unsigned pos = 0;
  unsigned len = 0;

  inline void init(unsigned N, unsigned startPos = 0) {
    len = (N > 0) ? N : 0;
    pos = startPos;
    const float d = (len > 1) ? (kFastCrossfadeHalfPi / float(len - 1)) : 0.f;
    sd = static_cast<float>(std::sin(d));
    cd = static_cast<float>(std::cos(d));
    s = 0.f; c = 1.f;
    for (unsigned i = 0; i < startPos; ++i) {
      const float s0 = s, c0 = c;
      s = s0 * cd + c0 * sd;
      c = c0 * cd - s0 * sd;
    }
  }

  inline void next(float &a, float &b) {
    const float aa = c * c;
    const float bb = s * s;
    a = aa;
    b = bb;
    const float s0 = s, c0 = c;
    s = s0 * cd + c0 * sd;
    c = c0 * cd - s0 * sd;
    if (pos < len) ++pos;
  }
};

struct CFSqrtEP {
  // fallback sqrt stepper using shared LUT
  const float* lut = nullptr;
  int lutSize = 0; // kSize
  float idx = 0.f;
  float didx = 0.f;
  unsigned pos = 0;
  unsigned len = 0;

  inline void init(unsigned N, unsigned startPos = 0) {
    len = (N > 0) ? N : 0;
    pos = startPos;
    lut = FastSqrtLUT::table();
    lutSize = FastSqrtLUT::kSize;
    // map length [0..len-1] to [0..lutSize]
    didx = (len > 1) ? float(lutSize) / float(len - 1) : 0.f;
    idx = float(startPos) * didx;
  }

  inline void next(float &a, float &b) {
    int i = int(idx);
    if (i < 0) i = 0;
    if (i > lutSize) i = lutSize;
    const float f = idx - float(i);
    const float v0 = lut[i];
    const float v1 = (i < lutSize) ? lut[i + 1] : lut[lutSize];
    const float rt = v0 + (v1 - v0) * f;
    int ii = lutSize - i;
    if (ii < 0) ii = 0;
    if (ii > lutSize) ii = lutSize;
    const float u0 = lut[ii];
    const float u1 = (ii > 0) ? lut[ii - 1] : lut[0];
    const float r1 = u0 + (u1 - u0) * f;
    a = r1;
    b = rt;
    idx += didx;
    if (pos < len) ++pos;
  }
};

// --- Template cache for precomputed (a,b) arrays per (mode,len) ---
// This precomputes the a/b gains once per mode+length and caches them.
// Lookup by key(mode,len) returns shared_ptr to template with vectors a,b.

struct FastXfadeTemplate {
  unsigned len = 0;
  std::vector<float> a; // out gains (a)
  std::vector<float> b; // in gains (b)
  FastXfadeTemplate(unsigned L = 0) : len(L), a(L), b(L) {}
};

inline uint64_t make_key(GOCrossfadeMode m, unsigned len) {
  return (uint64_t(uint8_t(m)) << 32) | uint64_t(len);
}

class FastCrossfadeCache {
private:
  static std::unordered_map<uint64_t, std::shared_ptr<FastXfadeTemplate>> s_cache;
  static std::mutex s_mutex;

  static void build_template_locked(GOCrossfadeMode mode, unsigned len) {
    uint64_t k = make_key(mode, len);
    if (s_cache.find(k) != s_cache.end()) return;
    auto tpl = std::make_shared<FastXfadeTemplate>(len);
    if (len == 0) {
      s_cache[k] = tpl;
      return;
    }
    for (unsigned i = 0; i < len; ++i) {
      const float t = (len > 1) ? float(i) / float(len - 1) : 1.0f;
      // use existing evaluator as reference to build arrays
      const auto g = go_crossfade_eval(mode, t);
      tpl->a[i] = g.a;
      tpl->b[i] = g.b;
    }
    s_cache[k] = tpl;
  }

public:
  // Ensure template exists (build on demand)
  static void EnsureTemplate(GOCrossfadeMode mode, unsigned len) {
    uint64_t k = make_key(mode, len);
    {
      std::lock_guard<std::mutex> lk(s_mutex);
      if (s_cache.find(k) != s_cache.end()) return;
    }
    // build without holding lock to avoid long holds (but protect map insertion)
    std::shared_ptr<FastXfadeTemplate> tpl;
    {
      std::lock_guard<std::mutex> lk(s_mutex);
      if (s_cache.find(k) != s_cache.end()) return;
      // Build directly under lock is acceptable here; length lists are small.
      build_template_locked(mode, len);
    }
  }

  // Precompute common lengths for a mode (call at startup or on mode change)
  static void PrecomputeForMode(GOCrossfadeMode mode, const std::vector<unsigned>& lengths) {
    for (unsigned L : lengths) EnsureTemplate(mode, L);
  }

  static std::shared_ptr<FastXfadeTemplate> GetTemplate(GOCrossfadeMode mode, unsigned len) {
    uint64_t k = make_key(mode, len);
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_cache.find(k);
    if (it == s_cache.end()) return nullptr;
    return it->second;
  }
};

// static members
std::unordered_map<uint64_t, std::shared_ptr<FastXfadeTemplate>> FastCrossfadeCache::s_cache;
std::mutex FastCrossfadeCache::s_mutex;

} // namespace GOAudioParams
