/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

#include "GOCpuPlacementProbe.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <sched.h>
#endif

int GOCpuPlacementProbe::GetCurrentCpu() {
#ifdef _WIN32
  return (int)GetCurrentProcessorNumber();
#elif defined(__linux__)
  return sched_getcpu();
#else
  return -1;
#endif
}
