#ifndef TIDES2_MODULATORS_FORMANT_H_
#define TIDES2_MODULATORS_FORMANT_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/parameter_interpolator.h"
#include "stmlib/dsp/polyblep.h"

#include "stmlib/dsp/units.h"

#include "tides2/io_buffer.h"
#include "tides2/poly_slope_generator.h"
#include "tides2/resources.h"

namespace tides {

using namespace stmlib;

class FormantEngine {
 public:
  FormantEngine() { }
  ~FormantEngine() { }

  void Init() {
    driver_phase_ = 0.0f;
    env_phase_ = 1.0f;  // start idle
    prev_f0_ = 0.001f;
    prev_shift_ = 0.5f;
    prev_pw_ = 0.5f;
    prev_shape_ = 0.0f;
    next_sample_pulse_ = 0.0f;
    next_sample_hollow_ = 0.0f;
    next_sample_sub_ = 0.0f;
    sub_state_ = false;
    prev_hollow_state_ = 0;
    std::fill(&lp_1_[0], &lp_1_[2], 0.0f);
    std::fill(&lp_2_[0], &lp_2_[2], 0.0f);
  }

  void Render(
      const Parameters& parameters,
      float f0,
      float base_freq,
      PolySlopeGenerator::OutputSample* out,
      const stmlib::GateFlags* gate_flags,
      bool trig_patched,
      bool alt_mode,
      size_t size) {

    // Map shift to formant ratio: +-48 semitones (8 octaves)
    float shift = parameters.shift;
    CONSTRAIN(shift, 0.0f, 1.0f);
    float target_ratio = stmlib::SemitonesToRatio((shift - 0.5f) * 96.0f);

    float pw = parameters.slope;
    CONSTRAIN(pw, 0.05f, 0.95f);

    // Shape maps to wavetable index (0-3.999 range for AD-style shapes)
    float shape = parameters.shape * 3.9999f;

    float smoothness = parameters.smoothness;
    float fold = std::max(2.0f * (smoothness - 0.5f), 0.0f);
    CONSTRAIN(f0, 0.0f, 0.25f);

    ParameterInterpolator f0_mod(&prev_f0_, f0, size);
    ParameterInterpolator shift_mod(&prev_shift_, target_ratio, size);
    ParameterInterpolator pw_mod(&prev_pw_, pw, size);
    ParameterInterpolator shape_mod(&prev_shape_, shape, size);

    for (size_t i = 0; i < size; ++i) {
      const float f0 = f0_mod.Next();
      const float ratio = shift_mod.Next();
      const float pw = pw_mod.Next();
      const float shape = shape_mod.Next();

      const float formant_freq = alt_mode ? ratio * base_freq : f0 * ratio;

      // Driver oscillator
      bool gate_reset = gate_flags[i] & stmlib::GATE_FLAG_RISING;
      bool trigger = false;
      bool prev_sub = sub_state_;
      driver_phase_ += f0;

      if (trig_patched) {
        // External trigger drives envelope, driver free-runs
        if (driver_phase_ >= 1.0f) {
          driver_phase_ -= 1.0f;
          sub_state_ = !sub_state_;
        }
        if (gate_reset) {
          sub_state_ = !sub_state_;
          trigger = true;
        }
      } else {
        // Normal: driver wrap triggers envelope, gate does hard sync
        if (gate_reset) {
          driver_phase_ = 0.0f;
          sub_state_ = false;
          trigger = true;
        }
        if (driver_phase_ >= 1.0f) {
          driver_phase_ -= 1.0f;
          sub_state_ = !sub_state_;
          trigger = true;
        }
      }

      if (trigger) {
        if (env_phase_ >= pw) {
          // In decay or idle: restart attack from current level
          float current_level = 0.0f;
          if (env_phase_ < 1.0f) {
            current_level = 1.0f - (env_phase_ - pw) / (1.0f - pw);
          }
          env_phase_ = current_level * pw;
        }
        // else: still in attack, ignore 
      }

      env_phase_ += formant_freq;
      if (env_phase_ > 1.0f) {
        env_phase_ = 1.0f;  // clamp, don't wrap (AD behavior)
      }

      float raw_level;
      if (env_phase_ < pw) {
        raw_level = env_phase_ / pw;
      } else if (env_phase_ < 1.0f) {
        raw_level = 1.0f - (env_phase_ - pw) / (1.0f - pw);
      } else {
        raw_level = 0.0f;
      }

      MAKE_INTEGRAL_FRACTIONAL(shape);
      const int16_t* shape_table = &lut_wavetable[shape_integral * 1025];

      float ws_index = 1024.0f * raw_level;
      MAKE_INTEGRAL_FRACTIONAL(ws_index);
      ws_index_integral &= 1023;
      float x0 = static_cast<float>(shape_table[ws_index_integral]) / 32768.0f;
      float x1 = static_cast<float>(shape_table[ws_index_integral + 1]) / 32768.0f;
      float y0 = static_cast<float>(shape_table[ws_index_integral + 1025]) / 32768.0f;
      float y1 = static_cast<float>(shape_table[ws_index_integral + 1026]) / 32768.0f;
      float x = x0 + (x1 - x0) * ws_index_fractional;
      float y = y0 + (y1 - y0) * ws_index_fractional;
      float shaped = x + (y - x) * shape_fractional;

      // Formant output
      {
        float bipolar = 2.0f * shaped - 1.0f;
        if (fold > 0.0f) {
          float folded = stmlib::Interpolate(
              lut_bipolar_fold,
              0.5f + bipolar * (0.03f + 0.46f * fold),
              1024.0f);
          bipolar = bipolar + (folded - bipolar) * fold;
        }
        out[i].channel[0] = 5.0f * bipolar;
      }

      // Hollow output
      {
        // Determine current hollow state: +1 attack, -1 decay, 0 idle
        int hollow_state;
        if (env_phase_ >= 1.0f || raw_level < 0.001f) {
          hollow_state = 0;   // idle
        } else if (env_phase_ < pw) {
          hollow_state = 1;   // attacking
        } else {
          hollow_state = -1;  // decaying
        }

        float this_sample = next_sample_hollow_;
        float next_sample = 0.0f;

        // BLEP on state transitions
        if (hollow_state != prev_hollow_state_) {
          float discontinuity = static_cast<float>(hollow_state - prev_hollow_state_);
          float t = 0.0f;
          this_sample += stmlib::ThisBlepSample(t) * discontinuity * 0.5f;
          next_sample += stmlib::NextBlepSample(t) * discontinuity * 0.5f;
        }

        next_sample += static_cast<float>(hollow_state);
        next_sample_hollow_ = next_sample;
        prev_hollow_state_ = hollow_state;

        // Sub-octave BLEP: always run to keep state fresh
        float sub_sample = next_sample_sub_;
        float sub_next = 0.0f;
        if (sub_state_ != prev_sub) {
          float discontinuity = sub_state_ ? 1.0f : -1.0f;
          float t = 0.0f;
          sub_sample += stmlib::ThisBlepSample(t) * discontinuity;
          sub_next += stmlib::NextBlepSample(t) * discontinuity;
        }
        sub_next += sub_state_ ? 1.0f : 0.0f;
        next_sample_sub_ = sub_next;

        // Smoothness: CCW attenuates, CW mixes in sub-octave square
        float hollow_out = this_sample;
        if (smoothness < 0.5f) {
          hollow_out *= smoothness * 2.0f;
        } else if (smoothness > 0.5f) {
          float sub_bipolar = 2.0f * sub_sample - 1.0f;
          float sub_amount = (smoothness - 0.5f) * 2.0f;
          hollow_out = this_sample + sub_bipolar * sub_amount;
        }
        out[i].channel[1] = 5.0f * hollow_out;
      }

      // Driver pulse output
      {
        float this_sample = next_sample_pulse_;
        float next_sample = 0.0f;

        float phase = driver_phase_;

        // BLEP at phase wrap (0 crossing, rising edge)
        if (phase < f0) {
          float t = phase / f0;
          this_sample += stmlib::ThisBlepSample(t);
          next_sample += stmlib::NextBlepSample(t);
        }

        // BLEP at pw crossing (falling edge)
        float d = phase - pw;
        if (d >= 0.0f && d < f0) {
          float t = d / f0;
          this_sample -= stmlib::ThisBlepSample(t);
          next_sample -= stmlib::NextBlepSample(t);
        }

        next_sample += phase < pw ? 1.0f : 0.0f;
        next_sample_pulse_ = next_sample;
        out[i].channel[2] = 10.0f * this_sample - 5.0f;
      }

      // Sine output
      out[i].channel[3] = 5.0f * stmlib::Interpolate(
          lut_sine, driver_phase_, 1024.0f);
    }

    // Smoothness filtering
    if (smoothness < 0.5f) {
      float ratio = smoothness * 2.0f;
      ratio *= ratio * ratio;
      for (size_t s = 0; s < size; ++s) {
        float f = f0 * 0.5f;
        f += (1.0f - f) * ratio;
        ONE_POLE(lp_1_[0], out[s].channel[0], f);
        ONE_POLE(lp_2_[0], lp_1_[0], f);
        out[s].channel[0] = lp_2_[0];
      }
    }
  }

 private:
  float driver_phase_;
  float env_phase_;

  float next_sample_pulse_;
  float next_sample_hollow_;
  float next_sample_sub_;
  bool sub_state_;
  int prev_hollow_state_;

  float prev_f0_;
  float prev_shift_;
  float prev_pw_;
  float prev_shape_;

  float lp_1_[2];
  float lp_2_[2];

  DISALLOW_COPY_AND_ASSIGN(FormantEngine);
};

}  // namespace tides

#endif  // TIDES2_MODULATORS_FORMANT_H_
