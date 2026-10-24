/*
 * GrandOrgue - free pipe organ simulator based on MyOrgan
 *
 * Copyright 2006 Milan Digital Audio LLC
 * Copyright 2009-2026 GrandOrgue contributors (see AUTHORS)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "GOSoundResample.h"

#include <cassert>
#include <math.h>
#include <stdlib.h>
#include <vector>

#include "sound/GOSoundDefs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846264338327950288
#endif

void GOSoundResample::ResamplingPosition::Init(
  float factor, unsigned startIndex, const ResamplingPosition *pOld) {
  m_index = startIndex;
  m_fraction = pOld ? pOld->m_fraction : 0;
  m_FractionIncrement
    = roundf(factor * (pOld ? pOld->m_FractionIncrement : UPSAMPLE_FACTOR));
}

static double sinc(const double arg) {
  return (arg == 0) ? 1.0 : sin(M_PI * arg) / (M_PI * arg);
}

#if 0
static
void
create_sinc_filter
	(float          *filter_buffer
	,const unsigned  filter_buffer_length
	,const float     f0
	,const float     band_width
	,const unsigned  sample_rate
	,const float     gain
	)
{
	for (unsigned i = 0; i < filter_buffer_length; i++)
	{
		double t = ((double)i - (filter_buffer_length * 0.5)) / sample_rate;
		filter_buffer[i] = gain * (float)(2.0 * band_width * cos(2.0 * M_PI * f0 * t) * sinc(band_width * t) / sample_rate);
	}
}
#endif

static void apply_lanczos_window(float *buffer, const unsigned length) {
  const int ldec = ((int)length) - 1;
  for (int i = 0; i <= ldec; i++) {
    buffer[i] *= sinc((2 * i - ldec) / (double)ldec);
  }
}

static void create_nyquist_filter(
  float *output, unsigned subfilter_taps, unsigned nb_subfilters) {
  int i;
  const int filter_length = (int)(subfilter_taps * nb_subfilters);
  const double inv_taps = 1.0 / nb_subfilters;
  for (i = 0; i < filter_length; i++) {
    double a = (i - filter_length / 2) * inv_taps * M_PI;
    double f = (i - filter_length / 2) ? (sin(a) / a) : 1.0;
    output[i] = f;
  }
}

GOSoundResample::GOSoundResample() {
  // Heap, not stack: UPSAMPLE_FACTOR * POLYPHASE_POINTS floats are 256 kB,
  // which is most of a default 1 MB thread stack (and more than some
  // secondary threads get at all). Built once at construction, so the
  // allocation is free of consequence.
  std::vector<float> tempStorage(UPSAMPLE_FACTOR * POLYPHASE_POINTS);
  float *const temp = tempStorage.data();

  create_nyquist_filter(temp, POLYPHASE_POINTS, UPSAMPLE_FACTOR);
  apply_lanczos_window(temp, UPSAMPLE_FACTOR * POLYPHASE_POINTS);

  /* Split up the filter into the sub-filters and reverse the coefficient
   * arrays. */
  for (unsigned i = 0; i < UPSAMPLE_FACTOR; i++) {
    for (unsigned j = 0; j < POLYPHASE_POINTS; j++) {
      m_PolyphaseCoefs[i][(POLYPHASE_POINTS - 1) - j]
        = temp[j * UPSAMPLE_FACTOR + i];
    }
  }
  for (unsigned i = 0; i < UPSAMPLE_FACTOR; i++) {
    m_LinearCoefs[i][0] = 1 - (i / (float)UPSAMPLE_FACTOR);
    m_LinearCoefs[i][1] = i / (float)UPSAMPLE_FACTOR;
  }

#if GO_POLYPHASE_SMALL_BITS
  /* Mini table: decimated rows of the full table, plus the virtual
   * top-neighbour row for the lerp - polyphase wrap identity
   * coefs[p + UPSAMPLE_FACTOR][k] = coefs[p][k-1]; k = 0 lies beyond the
   * filter's end where the Lanczos window is 0. See the member comment. */
  for (unsigned i = 0; i < SMALL_ROWS; i++)
    for (unsigned k = 0; k < POLYPHASE_POINTS; k++)
      m_PolyphaseSmall[i][k] = m_PolyphaseCoefs[i << SMALL_SHIFT][k];
  for (unsigned k = 0; k < POLYPHASE_POINTS; k++)
    m_PolyphaseSmall[SMALL_ROWS][k]
      = k >= 1 ? m_PolyphaseCoefs[0][k - 1] : 0.0f;
#endif
}

unsigned GOSoundResample::getVectorLength(
  GOSoundResample::InterpolationType interpolation) {
  unsigned res;

  switch (interpolation) {
  case GO_LINEAR_INTERPOLATION:
    res = LinearResampler::VECTOR_LENGTH;
    break;
  case GO_POLYPHASE_INTERPOLATION:
    res = PolyphaseResampler::VECTOR_LENGTH;
    break;

  /* A compiler cannot guarantee that interpolation has a valid value.
   * So for avoiding the "res may be used uninitialized" warning */
  default:
    assert(false);
    res = 0;
  }
  return res;
}

float *GOSoundResample::NewResampledMono(
  const float *data,
  unsigned &len,
  unsigned from_samplerate,
  unsigned to_samplerate) {
  ResamplingPosition resamplingPos;

  resamplingPos.Init((float)from_samplerate / to_samplerate);

  unsigned new_len = resamplingPos.AvailableTargetSamples(len);

  if (!new_len)
    return NULL;

  float *out = (float *)malloc(sizeof(float) * new_len);

  if (!out)
    return NULL;

  BoundedPtrSampleVector<float, float, 1> w(data, len);
  PolyphaseResampler resampler(*this);

  resampler.ResampleBlock<BoundedPtrSampleVector<float, float, 1>, 1>(
    resamplingPos, w, out, new_len);
  len = new_len;
  return out;
}
