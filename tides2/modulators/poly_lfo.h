// Copyright 2013 Emilie Gillet.
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
// Poly LFO.

#ifndef TIDES_POLY_LFO_H_
#define TIDES_POLY_LFO_H_

#include <algorithm>

#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"
#include "tides2/resources.h"
#include "tides2/modulators/wavetable_engine.h"


namespace tides {
using namespace stmlib;
using namespace std;

const uint8_t kNumChannels = 4;
const uint8_t num_waves = 17;
const uint16_t wavetable_size = 257;

inline float InterpolateWaveCubic(const uint8_t* table, int32_t index_integral, float index_fractional) {

    // Calculate indices for the points involved in the interpolation
    int xm1 = (index_integral - 1 + wavetable_size) % wavetable_size;
    int xp1 = (index_integral + 1) % wavetable_size;
    int xp2 = (index_integral + 2) % wavetable_size;

    // Since x is used directly as an index, ensure it is within the bounds of the array
    index_integral = index_integral % wavetable_size;

    // Retrieve the values from the table for the points involved in the interpolation
    const float ym1 = table[xm1];
    const float y0 = table[index_integral];
    const float y1 = table[xp1];
    const float y2 = table[xp2];

    // Perform the cubic interpolation
    const float c0 = y0;
    const float c1 = 0.5f * (y1 - ym1);
    const float c2 = ym1 - 2.5f * y0 + 2.0f * y1 - 0.5f * y2;
    const float c3 = 0.5f * (y2 - ym1) + 1.5f * (y0 - y1);

    // Calculate the interpolated value
    return ((c3 * index_fractional + c2) * index_fractional + c1) * index_fractional + c0;
}

#define WAVE(wave) &wt_lfo_waveforms[wave * 257]

const uint8_t* const wavetable[] = {
  WAVE(0),
  WAVE(1),
  WAVE(2),
  WAVE(3),
  WAVE(4),
  WAVE(5),
  WAVE(6),
  WAVE(7),
  WAVE(8),
  WAVE(9),
  WAVE(10),
  WAVE(11),
  WAVE(12),
  WAVE(13),
  WAVE(14),
  WAVE(15),
  WAVE(16)
};

class PolyLfo {
 public:
  PolyLfo() { }
  ~PolyLfo() { }
  
  void Init() {
    spread_ = 0.0f;
    shape_ = 0.0f;
    shape_spread_ = 0.0f;
    coupling_ = 0.0f;
    lp_ = 0.0f;
    frequency_ = 0.0f;
    waveform_ = 0.0f;
    
    fill(&value_[0], &value_[kNumChannels], 0.0f);
    for (int i = 0; i < kNumChannels; i++) {
      phase_[i] = 0.0f;
    }
  }

  void Render(float frequency, PolySlopeGenerator::OutputSample* out, const GateFlags* gate_flags, size_t size) {
    float max_index = float(num_waves - 1);
    float waveform = shape_ * max_index;
    const uint8_t* sine = &wt_lfo_waveforms[17 * 257];  

    stmlib::ParameterInterpolator waveform_modulation(&waveform_, waveform, size);
    stmlib::ParameterInterpolator f0_modulation(&frequency_, frequency, size);
 
    for (size_t index = 0; index < size; index++) {
      float wave = waveform_modulation.Next();
      float f0 = f0_modulation.Next();

      bool reset = false;
      if (gate_flags[index] & stmlib::GATE_FLAG_RISING) {
        std::fill(&phase_[0], &phase_[kNumChannels], 0.0f);
        reset = true;
      }

      for (uint8_t i = 0; i < kNumChannels; ++i) {
        MAKE_INTEGRAL_FRACTIONAL(wave);
        if (!reset) {
          if (spread_ > 0.5f) {
            float phase_difference = (spread_ - 0.5f);
            phase_[i] = i == 0 ? phase_[i] + f0 : phase_[i - 1] + phase_difference;
          } else {
            float spread = 2.0f * (1.0f - spread_);
            phase_[i] += f0;
            f0 *= spread;
          }

          if (phase_[i] >= 1.0f) {
            phase_[i] -= 1.0f;
          }
        }

        float phase = phase_[i];
        if (coupling_ > 0.0f) {
          phase += value_[(i + 1) % kNumChannels] * coupling_;
        } else {
          phase += value_[(i + kNumChannels - 1) % kNumChannels] * -coupling_;
        }
        if (phase >= 1.0f) {
          phase -= 1.0f;
        }
        
        const float p = phase * float(wavetable_size);
        MAKE_INTEGRAL_FRACTIONAL(p);
        
        const float x0 = InterpolateWaveCubic(wavetable[wave_integral % num_waves], p_integral, p_fractional);
        const float x1 = InterpolateWaveCubic(wavetable[(wave_integral + 1) % num_waves], p_integral, p_fractional);
        
        float s = (x0 + (x1 - x0) * wave_fractional);

        value_[i] = InterpolateWaveCubic(sine, p_integral, p_fractional) / 512.0f;
        out[index].channel[i] = s; 

        wave += shape_spread_ * max_index;
        if (wave < 0) {
          wave += num_waves;
        }
      }

      out[index].channel[3] = 0.0f;
      for (size_t i = 0; i < kNumChannels - 1; i++) {
        out[index].channel[3] += out[index].channel[i] / 3.0f;
      }
    }
  }

  inline void set_shape(float shape) {
    shape_ = shape;
  }

  inline void set_shape_spread(float shape_spread) {
    shape_spread_ = (shape_spread - 0.5f) / 2.0f;
  }

  inline void set_spread(float spread) {
    spread_ = spread;
  }

  inline void set_coupling(float coupling) {
    coupling_ = coupling - 0.5f;
  }

 private:
  float shape_;
  float shape_spread_;
  float spread_;
  float coupling_;

  float frequency_;
  float waveform_;

  float lp_;

  float value_[kNumChannels];
  float phase_[kNumChannels];

  DISALLOW_COPY_AND_ASSIGN(PolyLfo);
};

}  // namespace tides

#endif  // TIDES_POLY_LFO_H_
