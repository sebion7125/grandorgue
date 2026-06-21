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
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

// minimal dependency to the project's enum header
#include "GOCrossfadeMode.h"

// Debug dump switch: uncomment to enable writing template/reference/summary
// files next to the working directory. Enable only for debugging.
//#define GO_FAST_XFADE_DUMP 1

#ifdef GO_FAST_XFADE_DUMP
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#endif

namespace GOAudioParams {
static constexpr float kFastCrossfadeHalfPi = 1.57079632679489661923f;

// --- shared sqrt LUT (used by SqrtEqualPower) ---
struct FastSqrtLUT {
  static constexpr int kSize = 1024;
  // returns pointer to table of kSize+1 floats, values sqrt(i/kSize)
  static const float *table() {
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
    if (len == 0) {
      a = 1.0f;
      b = 0.0f;
      return;
    }
    if (pos >= (len - 1)) {
      a = 0.0f;
      b = 1.0f;
      return;
    }
    a = 1.0f - t;
    b = t;
    t += dt;
    if (pos < len)
      ++pos;
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
    if (len == 0) {
      a = 1.0f;
      b = 0.0f;
      return;
    }
    if (pos >= (len - 1)) {
      a = 0.0f;
      b = 1.0f;
      return;
    }
    const float tt = t * t;
    a = 1.0f - tt;
    b = tt;
    t += dt;
    if (pos < len)
      ++pos;
  }
};

struct CFSinEP {
  // recurrence values (fallback)
  float s = 0.f;  // sin(theta)
  float c = 1.f;  // cos(theta)
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
    // direct initialization at absolute position (no O(startPos) loop)
    if (len > 1) {
      const float theta = d * float(startPos);
      s = static_cast<float>(std::sin(theta));
      c = static_cast<float>(std::cos(theta));
    } else {
      s = 0.f;
      c = 1.f;
    }
  }

  inline void next(float &a, float &b) {
    if (len == 0) {
      a = 1.0f;
      b = 0.0f;
      return;
    }
    if (pos >= (len - 1)) {
      a = 0.0f;
      b = 1.0f;
      return;
    }
    a = c;
    b = s;
    const float s0 = s, c0 = c;
    s = s0 * cd + c0 * sd;
    c = c0 * cd - s0 * sd;
    if (pos < len)
      ++pos;
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
    // direct initialization at absolute position (no O(startPos) loop)
    if (len > 1) {
      const float theta = d * float(startPos);
      s = static_cast<float>(std::sin(theta));
      c = static_cast<float>(std::cos(theta));
    } else {
      s = 0.f;
      c = 1.f;
    }
  }

  inline void next(float &a, float &b) {
    if (len == 0) {
      a = 1.0f;
      b = 0.0f;
      return;
    }
    if (pos >= (len - 1)) {
      a = 0.0f;
      b = 1.0f;
      return;
    }
    const float aa = c * c;
    const float bb = s * s;
    a = aa;
    b = bb;
    const float s0 = s, c0 = c;
    s = s0 * cd + c0 * sd;
    c = c0 * cd - s0 * sd;
    if (pos < len)
      ++pos;
  }
};

struct CFSqrtEP {
  // fallback sqrt stepper using shared LUT
  const float *lut = nullptr;
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
    if (len == 0) {
      a = 1.0f;
      b = 0.0f;
      return;
    }
    if (pos >= (len - 1)) {
      a = 0.0f;
      b = 1.0f;
      return;
    }
    int i = int(idx);
    if (i < 0)
      i = 0;
    if (i > lutSize)
      i = lutSize;
    const float f = idx - float(i);
    const float v0 = lut[i];
    const float v1 = (i < lutSize) ? lut[i + 1] : lut[lutSize];
    const float rt = v0 + (v1 - v0) * f;
    int ii = lutSize - i;
    if (ii < 0)
      ii = 0;
    if (ii > lutSize)
      ii = lutSize;
    const float u0 = lut[ii];
    const float u1 = (ii > 0) ? lut[ii - 1] : lut[0];
    const float r1 = u0 + (u1 - u0) * f;
    a = r1;
    b = rt;
    idx += didx;
    if (pos < len)
      ++pos;
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
  inline static std::unordered_map<uint64_t, std::shared_ptr<FastXfadeTemplate>>
    s_cache{};
  inline static std::mutex s_mutex{};

  static void build_template_locked(GOCrossfadeMode mode, unsigned len) {
    uint64_t k = make_key(mode, len);
    if (s_cache.find(k) != s_cache.end())
      return;
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
      if (s_cache.find(k) != s_cache.end())
        return;
    }
    // build without holding lock to avoid long holds (but protect map
    // insertion)
    std::shared_ptr<FastXfadeTemplate> tpl;
    {
      std::lock_guard<std::mutex> lk(s_mutex);
      if (s_cache.find(k) != s_cache.end())
        return;
      // Build directly under lock is acceptable here; length lists are small.
      build_template_locked(mode, len);
    }
  }

  // Precompute common lengths for a mode (call at startup or on mode change)
  static void PrecomputeForMode(
    GOCrossfadeMode mode, const std::vector<unsigned> &lengths) {
    for (unsigned L : lengths) {
      EnsureTemplate(mode, L);

#ifdef GO_FAST_XFADE_DUMP
      // Dump template, reference and summary for offline inspection.
      auto tpl = GetTemplate(mode, L);
      if (tpl) {
        // Build a simple base filename using mode and length
        std::ostringstream base;
        base << "xfade_" << static_cast<int>(mode) << "_len" << L;

        // Timestamp for reproducibility
        const auto now = std::chrono::system_clock::now();
        const std::time_t now_c = std::chrono::system_clock::to_time_t(now);
        char timebuf[64];
        std::strftime(
          timebuf, sizeof(timebuf), "%Y%m%d-%H%M%S", std::localtime(&now_c));

        // Template file (actual cached a/b)
        {
          std::string fn = base.str() + "_template_" + timebuf + ".txt";
          std::ofstream ofs(fn);
          if (ofs) {
            ofs << "# mode=" << static_cast<int>(mode) << " len=" << L
                << " timestamp=" << timebuf << "\n";
            ofs << "# i a b\n";
            for (unsigned i = 0; i < tpl->len; ++i) {
              ofs << i << " " << std::setprecision(9) << tpl->a[i] << " "
                  << tpl->b[i] << "\n";
            }
            ofs.close();
          }
        }

        // Reference file (exact evaluator)
        {
          std::string fn = base.str() + "_reference_" + timebuf + ".txt";
          std::ofstream ofs(fn);
          if (ofs) {
            ofs << "# mode=" << static_cast<int>(mode) << " len=" << L
                << " timestamp=" << timebuf << "\n";
            ofs << "# i t a_ref b_ref\n";
            for (unsigned i = 0; i < tpl->len; ++i) {
              const float t
                = (tpl->len > 1) ? float(i) / float(tpl->len - 1) : 0.0f;
              const auto g = go_crossfade_eval(mode, t);
              ofs << i << " " << std::setprecision(9) << t << " " << g.a << " "
                  << g.b << "\n";
            }
            ofs.close();
          }
        }

        // Summary file (max/mean abs diffs)
        {
          std::string fn = base.str() + "_summary_" + timebuf + ".txt";
          std::ofstream ofs(fn);
          if (ofs) {
            double max_da = 0.0, max_db = 0.0;
            double sum_da = 0.0, sum_db = 0.0;
            for (unsigned i = 0; i < tpl->len; ++i) {
              const float t
                = (tpl->len > 1) ? float(i) / float(tpl->len - 1) : 0.0f;
              const auto g = go_crossfade_eval(mode, t);
              const double da = std::abs(double(tpl->a[i]) - double(g.a));
              const double db = std::abs(double(tpl->b[i]) - double(g.b));
              if (da > max_da)
                max_da = da;
              if (db > max_db)
                max_db = db;
              sum_da += da;
              sum_db += db;
            }
            const double mean_da
              = (tpl->len > 0) ? (sum_da / double(tpl->len)) : 0.0;
            const double mean_db
              = (tpl->len > 0) ? (sum_db / double(tpl->len)) : 0.0;
            ofs << "mode=" << static_cast<int>(mode) << " len=" << L
                << " timestamp=" << timebuf << "\n";
            ofs << "max_abs_diff_a=" << std::setprecision(9) << max_da
                << " max_abs_diff_b=" << max_db << "\n";
            ofs << "mean_abs_diff_a=" << std::setprecision(9) << mean_da
                << " mean_abs_diff_b=" << mean_db << "\n";
            ofs.close();
          }
        }
      }
#endif
    }
  }

  static std::shared_ptr<FastXfadeTemplate> GetTemplate(
    GOCrossfadeMode mode, unsigned len) {
    uint64_t k = make_key(mode, len);
    std::lock_guard<std::mutex> lk(s_mutex);
    auto it = s_cache.find(k);
    if (it == s_cache.end())
      return nullptr;
    return it->second;
  }
};

} // namespace GOAudioParams
