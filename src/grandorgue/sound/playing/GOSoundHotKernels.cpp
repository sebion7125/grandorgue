/*
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 * License GPL-2.0 or later
 * (https://www.gnu.org/licenses/old-licenses/gpl-2.0.html).
 */

/* The hot-kernel translation unit - see GOSoundHotKernels.h for the full
 * rationale. Defining every AVX2 render kernel here (instead of leaving
 * them as implicit COMDAT instantiations in their using TUs) puts them
 * all into THIS object's .text.hot, contiguously in pipeline order, so
 * the linker's hot-region grouping covers the kernels on PE and ELF
 * alike. */

#define GO_HOT_KERNELS_DEFINE
#include "GOSoundHotKernels.h"
