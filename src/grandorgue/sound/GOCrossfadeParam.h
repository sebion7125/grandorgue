#pragma once
#include <atomic>
#include "GOCrossfadeMode.h"

namespace GOAudioParams {
  inline std::atomic<uint8_t> g_crossfadeMode{
    static_cast<uint8_t>(GOCrossfadeMode::Linear) // Default as before
  };
  inline GOCrossfadeMode GetCrossfadeMode() {
    return static_cast<GOCrossfadeMode>(g_crossfadeMode.load(std::memory_order_relaxed));
  }
  inline void SetCrossfadeMode(GOCrossfadeMode m) {
    g_crossfadeMode.store(static_cast<uint8_t>(m), std::memory_order_relaxed);
  }

  // Fuse fade-and-accumulate runtime flag (default enabled)
  inline std::atomic<uint8_t> g_fuseFadeAccumulate{1};
  inline bool GetFuseFadeAndAccumulate() {
    return g_fuseFadeAccumulate.load(std::memory_order_relaxed) != 0;
  }
  inline void SetFuseFadeAndAccumulate(bool v) {
    g_fuseFadeAccumulate.store(v ? 1 : 0, std::memory_order_relaxed);
  }
}

// ── Release Alignment Mode ─────────────────────────────────────────────────
enum class GOReleaseAlignMode : uint8_t {
  Legacy      = 0,   // existing 2×32-bucket algorithm (instantaneous values)
  Correlation = 1,   // new LUT-based dot-product algorithm
};

namespace GOAudioParams {
  inline std::atomic<uint8_t> g_releaseAlignMode{
    static_cast<uint8_t>(GOReleaseAlignMode::Legacy)
  };
  inline GOReleaseAlignMode GetReleaseAlignMode() {
    return static_cast<GOReleaseAlignMode>(
      g_releaseAlignMode.load(std::memory_order_relaxed));
  }
  inline void SetReleaseAlignMode(GOReleaseAlignMode m) {
    g_releaseAlignMode.store(
      static_cast<uint8_t>(m), std::memory_order_relaxed);
  }

  // Correlation LUT downsampling for low-pitched pipes (default: enabled).
  // When enabled, pipes with large period T are computed on downsampled audio
  // (stride = T/500, max 4) to reduce load time with negligible quality loss.
  inline std::atomic<uint8_t> g_corrLutDownsampling{1};
  inline bool GetCorrLutDownsampling() {
    return g_corrLutDownsampling.load(std::memory_order_relaxed) != 0;
  }
  inline void SetCorrLutDownsampling(bool v) {
    g_corrLutDownsampling.store(v ? 1 : 0, std::memory_order_relaxed);
  }
}
