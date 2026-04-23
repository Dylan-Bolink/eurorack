// Copyright 2024 Dylan.
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
// Quad tides oscillator with per-output parameter shift.
//
// Output 1: shape, slope, smoothness (normal)
// Output 2: shift, slope, smoothness  (shift replaces shape)
// Output 3: shape, shift, smoothness  (shift replaces slope)
// Output 4: shape, slope, shift       (shift replaces smoothness)

#ifndef TIDES_SHIFTED_TIDES_H_
#define TIDES_SHIFTED_TIDES_H_

#include "stmlib/stmlib.h"
#include "tides2/io_buffer.h"
#include "tides2/poly_slope_generator.h"

namespace tides {

class ShiftedTides {
 public:
  ShiftedTides() { }
  ~ShiftedTides() { }

  void Init() {
    for (size_t j = 0; j < 4; ++j) {
      gen_[j].Init();
    }
  }

  void Render(
      Range range,
      float frequency,
      float shape,
      float slope,
      float smoothness,
      float shift,
      const stmlib::GateFlags* gate_flags,
      PolySlopeGenerator::OutputSample* out,
      size_t size) {

    const float shapes[4]       = { shape, shift, shape, shape };
    const float slopes[4]       = { slope, slope, shift, slope };
    const float smoothnesses[4] = { smoothness, smoothness, smoothness, shift };

    for (size_t j = 0; j < 4; ++j) {
      gen_[j].Render(
          RAMP_MODE_LOOPING,
          OUTPUT_MODE_SLOPE_PHASE,
          range,
          frequency,
          slopes[j],        // pw / slope
          shapes[j],        // shape
          smoothnesses[j],  // smoothness
          0.5f,             // no phase spread within each generator
          gate_flags,
          0,
          temp_,
          size);
      for (size_t i = 0; i < size; ++i) {
        out[i].channel[j] = temp_[i].channel[0];
      }
    }
  }

 private:
  PolySlopeGenerator gen_[4];
  PolySlopeGenerator::OutputSample temp_[kBlockSize];

  DISALLOW_COPY_AND_ASSIGN(ShiftedTides);
};

}  // namespace tides

#endif  // TIDES_SHIFTED_TIDES_H_
