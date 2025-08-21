#pragma once
#include <atomic>
#include "GOCrossfadeMode.h"

namespace GOAudioParams {
  inline std::atomic<uint8_t> g_crossfadeMode{
    static_cast<uint8_t>(GOCrossfadeMode::SinEqualPower) // Default as before
  };
  inline GOCrossfadeMode GetCrossfadeMode() {
    return static_cast<GOCrossfadeMode>(g_crossfadeMode.load(std::memory_order_relaxed));
  }
  inline void SetCrossfadeMode(GOCrossfadeMode m) {
    g_crossfadeMode.store(static_cast<uint8_t>(m), std::memory_order_relaxed);
  }
}
