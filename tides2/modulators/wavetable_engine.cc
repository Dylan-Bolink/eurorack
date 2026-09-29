// Copyright 2016 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
// 
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// 8x8x3 wave terrain.

#include "tides2/modulators/wavetable_engine.h"

#include <algorithm>

#include "tides2/resources.h"
#include "stmlib/dsp/parameter_interpolator.h"

namespace tides {

using namespace std;
using namespace stmlib;

extern const int16_t wav_integrated_waves[];

void WavetableEngine::Init() {
  
  x_pre_lp_ = 0.0f;
  y_pre_lp_ = 0.0f;
  z_pre_lp_ = 0.0f;

  previous_x_ = 0.0f;
  previous_y_ = 0.0f;
  previous_z_ = 0.0f;
  previous_f0_ = a0;
  fold_ = 0.0f;
  next_sample_ = 0.0f;
  phases_[0] = 0.0f;
  phases_[1] = 0.0f;
  next_sample_tri_ = 0.0f;
  diff_out_.Init();
  direct_lp_ = 0.0f;
  fill(&lp_1_[0], &lp_1_[4], 0.0f);
  fill(&lp_2_[0], &lp_2_[4], 0.0f);
}

inline const int16_t* Wave(int x, int y, int z) {
  const int idx = x + y * 8;
  return wav_integrated_waves + (z * 64 + idx) * (table_size + 4);
}

inline float ReadWave(
  int x,
  int y,
  int z,
  int phase_integral,
  float phase_fractional) {
  return InterpolateWaveHermite(
      Wave(x, y, z), phase_integral, phase_fractional);
}

inline float ReadWave(
  int x,
  int y,
  int z,
  int phase_integral,
  float phase_fractional,
  float* derivative) {
  return InterpolateWaveHermite(
      Wave(x, y, z), phase_integral, phase_fractional, derivative);
}

// Corners ordered x0y0z0, x1y0z0, x0y1z0, x1y1z0, x0y0z1, x1y0z1, x0y1z1,
// x1y1z1.
inline float Trilinear(
    const float* c,
    float x_fractional,
    float y_fractional,
    float z_fractional) {
  float xy0z0 = c[0] + (c[1] - c[0]) * x_fractional;
  float xy1z0 = c[2] + (c[3] - c[2]) * x_fractional;
  float xyz0 = xy0z0 + (xy1z0 - xy0z0) * y_fractional;
  float xy0z1 = c[4] + (c[5] - c[4]) * x_fractional;
  float xy1z1 = c[6] + (c[7] - c[6]) * x_fractional;
  float xyz1 = xy0z1 + (xy1z1 - xy0z1) * y_fractional;
  return xyz0 + (xyz1 - xyz0) * z_fractional;
}

// Below kDifferentiatorMinF0, differentiating the integrated wavetable loses
// precision: the per-sample change drops below float/int16 resolution while
// the 1 / f0 gain keeps growing. Down there the analytic derivative of the
// Hermite interpolant is read directly (aliasing is not a concern), with a
// crossfade between the two methods from kDirectReadMaxF0 up.
//
// This engine runs at half the sample rate (the SLOPE_PHASE output mode is
// half_speed in tides.cc), so f0 is normalized to kSampleRate / 2.
const float kEngineSampleRate = kSampleRate * 0.5f;
const float kDirectReadMaxF0 = 50.0f / kEngineSampleRate;
const float kDifferentiatorMinF0 = 100.0f / kEngineSampleRate;

void WavetableEngine::Render(
    const Parameters& parameters,
    float f0,
    PolySlopeGenerator::OutputSample* out,
    const GateFlags* gate_flags,
    bool alt_mode,
    size_t size) {
  
  ONE_POLE(x_pre_lp_, parameters.slope * 6.9999f, 0.2f);
  ONE_POLE(y_pre_lp_, parameters.shift * 6.9999f, 0.2f);
  const float kBreak = 0.85f;
  const float kPlaitsScale = 3.0f / kBreak;
  const float kNoiseScale  = 0.9999f / (1.0f - kBreak);
  float z_target = (parameters.shape < kBreak)
      ? parameters.shape * kPlaitsScale
      : 3.0f + (parameters.shape - kBreak) * kNoiseScale;
  ONE_POLE(z_pre_lp_, z_target, 0.05f);
  
  const float x = x_pre_lp_;
  const float y = y_pre_lp_;
  const float z = z_pre_lp_;
  
  MAKE_INTEGRAL_FRACTIONAL(x);
  MAKE_INTEGRAL_FRACTIONAL(y);
  MAKE_INTEGRAL_FRACTIONAL(z);
  
  if (alt_mode) {
    x_fractional = 0.0f;
    y_fractional = 0.0f;
    z_fractional = 0.0f;
  }

  ParameterInterpolator x_modulation(
      &previous_x_, static_cast<float>(x_integral) + x_fractional, size);
  ParameterInterpolator y_modulation(
      &previous_y_, static_cast<float>(y_integral) + y_fractional, size);
  ParameterInterpolator z_modulation(
      &previous_z_, static_cast<float>(z_integral) + z_fractional, size);

  ParameterInterpolator f0_modulation(&previous_f0_, f0, size);

  ParameterInterpolator fold_modulation(&fold_, max(2.0f * (parameters.smoothness - 0.5f), 0.0f), size);

  for (size_t index = 0; index < size; index++) {
    const float f0 = f0_modulation.Next();
    const float sub_f = f0 * 0.5f;

    bool reset = false;
    if (gate_flags[index] & stmlib::GATE_FLAG_RISING) {
      std::fill(&phases_[0], &phases_[2], 0.0f);
      reset = true;
    }
    
    const float cutoff = min(table_size_f * f0, 1.0f);

    const float x = x_modulation.Next();
    const float y = y_modulation.Next();
    const float z = z_modulation.Next();

    MAKE_INTEGRAL_FRACTIONAL(x);
    MAKE_INTEGRAL_FRACTIONAL(y);
    MAKE_INTEGRAL_FRACTIONAL(z);

    if (!reset) {
      for (int i = 0; i < 2; i++) {
        phases_[i] += i == 0 ? f0 : sub_f;
        if (phases_[i] >= 1.0f) {
          phases_[i] -= 1.0f;
        }
      }
    }
    
    const float p = phases_[0] * table_size;
    MAKE_INTEGRAL_FRACTIONAL(p);

    int x0 = x_integral;
    int x1 = x_integral + 1;
    int y0 = y_integral;
    int y1 = y_integral + 1;
    int z0 = z_integral;
    int z1 = z_integral + 1;

    if (z0 >= 4) {
      z0 = 3;
    }
    if (z1 >= 4) {
      z1 = 3;
    }

    float corners[8];
    float mix;
    if (f0 >= kDifferentiatorMinF0) {
      corners[0] = ReadWave(x0, y0, z0, p_integral, p_fractional);
      corners[1] = ReadWave(x1, y0, z0, p_integral, p_fractional);
      corners[2] = ReadWave(x0, y1, z0, p_integral, p_fractional);
      corners[3] = ReadWave(x1, y1, z0, p_integral, p_fractional);
      corners[4] = ReadWave(x0, y0, z1, p_integral, p_fractional);
      corners[5] = ReadWave(x1, y0, z1, p_integral, p_fractional);
      corners[6] = ReadWave(x0, y1, z1, p_integral, p_fractional);
      corners[7] = ReadWave(x1, y1, z1, p_integral, p_fractional);
      mix = Trilinear(corners, x_fractional, y_fractional, z_fractional);
      const float gain = (1.0f / (f0 * 131072.0f)) * (0.95f - f0);
      mix = diff_out_.Process(cutoff, mix) * gain;
    } else {
      float derivatives[8];
      corners[0] = ReadWave(x0, y0, z0, p_integral, p_fractional, &derivatives[0]);
      corners[1] = ReadWave(x1, y0, z0, p_integral, p_fractional, &derivatives[1]);
      corners[2] = ReadWave(x0, y1, z0, p_integral, p_fractional, &derivatives[2]);
      corners[3] = ReadWave(x1, y1, z0, p_integral, p_fractional, &derivatives[3]);
      corners[4] = ReadWave(x0, y0, z1, p_integral, p_fractional, &derivatives[4]);
      corners[5] = ReadWave(x1, y0, z1, p_integral, p_fractional, &derivatives[5]);
      corners[6] = ReadWave(x0, y1, z1, p_integral, p_fractional, &derivatives[6]);
      corners[7] = ReadWave(x1, y1, z1, p_integral, p_fractional, &derivatives[7]);

      // Keep the differentiator running so the crossfade has no transient.
      float differentiated = diff_out_.Process(
          cutoff,
          Trilinear(corners, x_fractional, y_fractional, z_fractional));

      // d(mix)/d(sample) = derivative * table_size * f0, times the gain.
      // Same one-pole lowpass as the differentiator, so both paths match.
      float direct = Trilinear(
          derivatives, x_fractional, y_fractional, z_fractional) *
          (table_size_f / 131072.0f) * (0.95f - f0);
      ONE_POLE(direct_lp_, direct, cutoff);
      mix = direct_lp_;

      // Select rather than lerp: the 1 / f0 gain is inf at f0 = 0 (clocked
      // with no period yet), and inf * 0 would be NaN.
      if (f0 > kDirectReadMaxF0) {
        const float gain = (1.0f / (f0 * 131072.0f)) * (0.95f - f0);
        const float amount = (f0 - kDirectReadMaxF0) /
            (kDifferentiatorMinF0 - kDirectReadMaxF0);
        mix += (differentiated * gain - mix) * amount;
      }
    }
    float fold_amount = fold_modulation.Next();

    out[index].channel[0] = fold(mix, fold_amount, true);
    out[index].channel[1] = fold(mix, fold_amount * 0.65f, false);
    out[index].channel[2] = mix >= 0.0f ? 5.0f : -5.0f;
    out[index].channel[3] = BandLimitedPulse(phases_[1], sub_f, 0.5f);
  }
  filter(f0, parameters.smoothness, out, size);
}

}  // namespace tides
