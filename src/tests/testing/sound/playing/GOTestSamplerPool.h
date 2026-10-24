/*
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#ifndef GOTESTSAMPLERPOOL_H
#define GOTESTSAMPLERPOOL_H

#include <string>

#include "GOTest.h"

class GOTestSamplerPool : public GOTest {
private:
  static const std::string TEST_NAME;

  /**
   * The pool's arena placement contract: every sampler and every voice
   * block lives at a cache-line-aligned address, consecutive slots sit
   * at a fixed stride whose line count is odd (coprime to the
   * power-of-two cache set count), and any 64 consecutive slots
   * therefore cover all 64 L1 set positions. Also checks the usage
   * limit gate, the freelist round trip, address stability across a
   * limit increase (new chunk, old pointers untouched), and voice-block
   * reattachment on a channel-stride change.
   */
  void TestArenaGeometry();

public:
  std::string GetName() override { return TEST_NAME; }
  void run() override;
};

#endif /* GOTESTSAMPLERPOOL_H */
