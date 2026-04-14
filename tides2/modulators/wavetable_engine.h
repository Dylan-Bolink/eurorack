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

#ifndef TIDES2_MODULATORS_ENGINE_WAVETABLE_ENGINE_H_
#define TIDES2_MODULATORS_ENGINE_WAVETABLE_ENGINE_H_

//#include "stmlib/stmlib.h"
#include "stmlib/utils/buffer_allocator.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"
#include "stmlib/dsp/filter.h"
#include "tides2/io_buffer.h"
#include "tides2/poly_slope_generator.h"

namespace tides {
using namespace stmlib;

const float a0 = (440.0f / 8.0f) / kSampleRate;
const size_t table_size = 128;
const float table_size_f = float(table_size);

class Differentiator {
 public:
  Differentiator() { }
  ~Differentiator() { }

  void Init() {
    previous_ = 0.0f;
    lp_ = 0.0f;
  }
  
  float Process(float coefficient, float s) {
    ONE_POLE(lp_, s - previous_, coefficient);
    previous_ = s;
    return lp_;
  }
 private:
  float lp_;
  float previous_;

  DISALLOW_COPY_AND_ASSIGN(Differentiator);
};

class WavetableEngine {
 public:
  WavetableEngine() { }
  ~WavetableEngine() { }
  
  virtual void Init();

  virtual void Render(const Parameters& parameters,
    float f0,
    PolySlopeGenerator::OutputSample* out,
    const GateFlags* gate_flags,
    size_t size);

  inline float fold(float value, float fold_amount, bool bipolar) {
    if (bipolar) {
      float folded = fold_amount > 0.0f ? stmlib::Interpolate(
          lut_bipolar_fold,
          0.5f + value * (0.03f + 0.46f * fold_amount),
          1024.0f) : 0.0f;
      return 5.0f * (value + (folded - value) * fold_amount);
    } else {
      value = (value + 1.0f) / 2.0f;
      float folded = fold_amount > 0.0f ? stmlib::Interpolate(
          lut_unipolar_fold,
          value * fold_amount,
          1024.0f) : 0.0f;
      return 8.0f * (value + (folded - value) * fold_amount);
    }
  }

  inline float warps_fold(float value, float fold_amount) {
    const float kScale = 2048.0f / ((1.0f + 1.0f + 0.25f) * 1.02f);
    float amount = 0.02f + fold_amount;
    float folded = Interpolate(lut_warps_bipolar_fold + 2048, value * amount, kScale) * -10.0f;
    return folded;
  }

  inline void filter(float frequency, float smoothness, PolySlopeGenerator::OutputSample* out, size_t size) {
    if (smoothness < 0.5f) {
      float ratio = smoothness * 2.0f;
      ratio *= ratio * ratio;

      for (size_t s = 0; s < size; s++) {
        for (size_t i = 0; i < 4; ++i) {
          float f = frequency * 0.5f;
          f += (1.0f - f) * ratio;// f = frequency * 0.5f * (1- ratio) + ratio
          ONE_POLE(lp_1_[i], out[s].channel[i], f);
          ONE_POLE(lp_2_[i], lp_1_[i], f);
          out[s].channel[i] = lp_2_[i];
        }
      }
    }
  }

  inline float BandLimitedPulse(float phase, float frequency, float pw) {
    CONSTRAIN(pw, frequency * 2.0f, 1.0f - 2.0f * frequency);
    
    float this_sample = next_sample_;
    float next_sample = 0.0f;
    
    float wrap_point = pw;
    if (phase < pw * 0.5f) {
      wrap_point = 0.0f;
    } else if (phase > 0.5f + pw * 0.5f) {
      wrap_point = 1.0f;
    }
    
    const float d = phase - wrap_point;
    
    if (d >= 0.0f && d < frequency) {
      const float t = d / frequency;
      float discontinuity = 1.0f;
      if (wrap_point != pw) {
        discontinuity = -discontinuity;
      }
      if (frequency < 0.0f) {
        discontinuity = -discontinuity;
      }
      this_sample += stmlib::ThisBlepSample(t) * discontinuity;
      next_sample += stmlib::NextBlepSample(t) * discontinuity;
    }
    
    next_sample += phase < pw ? 0.0f : 1.0f;
    next_sample_ = next_sample;
    
    return 10.0f * this_sample - 5.0f;
  }
  
  inline float BandLimitedSlope(float phase, float frequency, float pw) {
    
    CONSTRAIN(pw, fabsf(frequency) * 2.0f, 1.0f - 2.0f * fabsf(frequency));

    float this_sample = next_sample_tri_;
    float next_sample = 0.0f;
    
    float wrap_point = pw;
    if (phase < pw * 0.5f) {
      wrap_point = 0.0f;
    } else if (phase > 0.5f + pw * 0.5f) {
      wrap_point = 1.0f;
    }
    
    const float slope_up = 1.0f / pw;
    const float slope_down = 1.0f / (1.0f - pw);
    const float d = phase - wrap_point;
    
    if (d >= 0.0f && d < frequency) {
      const float t = d / frequency;
      float discontinuity = -(slope_up + slope_down) * frequency;
      if (wrap_point != pw) {
        discontinuity = -discontinuity;
      }
      if (frequency < 0.0f) {
        discontinuity = -discontinuity;
      }
      this_sample += stmlib::ThisIntegratedBlepSample(t) * discontinuity;
      next_sample += stmlib::NextIntegratedBlepSample(t) * discontinuity;
    }
    
    next_sample += phase < pw
      ? phase * slope_up
      : 1.0f - (phase - pw) * slope_down;
    next_sample_tri_ = next_sample;
    
    return 2.0f * this_sample - 1.0f;
  }

 private:
  float x_pre_lp_;
  float y_pre_lp_;
  float z_pre_lp_;

  float previous_x_;
  float previous_y_;
  float previous_z_;
  float previous_f0_;

  float fold_;
  float phases_[2];
  float next_sample_;
  float next_sample_tri_;

  float lp_1_[4];
  float lp_2_[4];

  Differentiator diff_out_;
  
  DISALLOW_COPY_AND_ASSIGN(WavetableEngine);
};

inline float InterpolateWaveHermite(
    const int16_t* table,
    int32_t index_integral,
    float index_fractional) {
  const float xm1 = table[index_integral];
  const float x0 = table[index_integral + 1];
  const float x1 = table[index_integral + 2];
  const float x2 = table[index_integral + 3];
  const float c = (x1 - xm1) * 0.5f;
  const float v = x0 - x1;
  const float w = c + v;
  const float a = w + v + (x2 - x0) * 0.5f;
  const float b_neg = w + a;
  const float f = index_fractional;
  return (((a * f) - b_neg) * f + c) * f + x0;
}

}  // namespace plaits

#endif  // TIDES2_MODULATORS_ENGINE_WAVETABLE_ENGINE_H_
