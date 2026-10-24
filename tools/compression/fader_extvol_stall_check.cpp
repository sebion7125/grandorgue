// fader_extvol_stall_check.cpp
//
// Proof for the "63% ramp in a hold test" mystery (report 2026-07-12
// 23:34): GOSoundFader smooths the external volume geometrically,
//   last += (target - last) * k,   k = nFrames/1024 = 0.125 at 128,
// and classifies the block as Constant only when last == target EXACTLY
// (GOSoundFader.cpp:619 and the ramp test at :407). In float, the
// increment (target-last)*k eventually rounds below 0.5 ulp of last -
// then += is a no-op, last is frozen a few ulp away from target, and the
// voice runs the Ramp kernel forever (dg ~ 1e-7: inaudible, pure cost).
//
// This simulates the exact update rule over the realistic gain range and
// counts how many (start, target) pairs converge to equality vs stall.
//
// Build: g++ -O2 -std=gnu++17 fader_extvol_stall_check.cpp -o /tmp/claude-1000/stall

#include <cmath>
#include <cstdio>
#include <random>

int main() {
  std::mt19937 rng(20260712);
  std::uniform_real_distribution<float> vol(0.05f, 4.0f);
  constexpr float k = 128.0f / 1024.0f;

  unsigned converged = 0, stalled = 0;
  double stallRelSum = 0, blocksToSettleSum = 0;
  const unsigned TRIALS = 200000;

  for (unsigned t = 0; t < TRIALS; t++) {
    float last = vol(rng);
    const float target = vol(rng); // new steady value after a change
    unsigned blocks = 0;

    while (target != last && blocks < 4000) { // 4000 blocks ~ 10.7 s
      const float next = last + (target - last) * k;
      if (next == last)
        break; // frozen: += is a no-op from here on
      last = next;
      blocks++;
    }
    if (target == last) {
      converged++;
      blocksToSettleSum += blocks;
    } else {
      stalled++;
      stallRelSum += std::fabs((double)target - last) / target;
    }
  }
  printf("update rule: last += (target-last) * %.3f  (128-frame blocks)\n", k);
  printf("trials    : %u random (start,target) volume pairs\n", TRIALS);
  printf("converged : %u (%.1f%%), avg %.0f blocks (%.0f ms) to settle\n",
    converged, 100.0 * converged / TRIALS,
    blocksToSettleSum / (converged ? converged : 1),
    blocksToSettleSum / (converged ? converged : 1) * 128.0 / 48.0);
  printf("STALLED   : %u (%.1f%%) - Ramp kernel forever, avg rel gap %.2e\n",
    stalled, 100.0 * stalled / TRIALS,
    stallRelSum / (stalled ? stalled : 1));
  return 0;
}
