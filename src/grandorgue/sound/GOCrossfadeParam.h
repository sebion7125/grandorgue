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
