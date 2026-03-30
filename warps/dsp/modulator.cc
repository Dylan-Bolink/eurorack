// Copyright 2014 Emilie Gillet.
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
// Modulator.

#include "warps/dsp/modulator.h"

#include <algorithm>

#include "stmlib/dsp/units.h"
#include "stmlib/utils/random.h"

#include "warps/drivers/debug_pin.h"
#include "warps/resources.h"

namespace warps {

using namespace std;
using namespace stmlib;

const float kXmodCarrierGain = 0.5f;
const float kSamplePeriod = 1.0f / 96000.0f;
const float kToFloat = 1.0f / 32768.0f;

inline float SkewRawAlgorithm(float r) {
    float x = r * 2.0f - 1.0f;
    float skewed = (x * x * x + 1.0f) * 0.5f;
    return skewed * 2.0f - 1.0f;
}

void Modulator::Init(float sample_rate) {
  bypass_ = false;
  feature_mode_ = FEATURE_MODE_META;

  for (int32_t i = 0; i < 2; ++i) {
    amplifier_[i].Init();
    src_up_[i].Init();
    src_up2_[i].Init();
    src_down2_[i].Init();
    quadrature_transform_[i].Init(lut_ap_poles, LUT_AP_POLES_SIZE);
  }
  src_down_.Init();

  xmod_oscillator_.Init(sample_rate);
  vocoder_oscillator_.Init(sample_rate);
  quadrature_oscillator_.Init(sample_rate);
  vocoder_.Init(sample_rate);

  previous_parameters_.carrier_shape = 0;
  previous_parameters_.channel_drive[0] = 0.0f;
  previous_parameters_.channel_drive[1] = 0.0f;
  previous_parameters_.modulation_algorithm = 0.0f;
  previous_parameters_.modulation_parameter = 0.0f;
  previous_parameters_.note = 48.0f;
  previous_parameters_.algorithm_frozen = false;
  previous_parameters_.frozen_algorithm = 0.0f;

  feedback_sample_ = 0.0f;
  delay_interpolation_ = INTERPOLATION_HERMITE;

  ShortFrame e = {0, 0};
  fill(delay_buffer_, delay_buffer_+DELAY_SIZE, e);

  filter_[0].Init();
  filter_[1].Init();
  filter_[2].Init();
  filter_[3].Init();
  filter_[4].Init();
  filter_[5].Init();
  filter_[6].Init();
  filter_[7].Init();

  // Tape effect filters
  shared_write_pos_ = 0;
  drift_decimation_active_ = false;
  drift_decimate_counter_ = 0;
  for (int i = 0; i < kSharedDelaySize; ++i) {
    delay_buffer_[i].l = 0;
    delay_buffer_[i].r = 0;
  }

  tape_lp_l_.Init();
  tape_lp_r_.Init();

  lossy_bpf_l_.Init();
  lossy_bpf_r_.Init();

  // Radio mixer
  radio_carrier_phase_ = 0.0f;
  radio_fade_lfo_phase_ = 0.0f;
  radio_static_lp_ = 0.0f;
}

void Modulator::ProcessFreqShifter(
    ShortFrame* input,
    ShortFrame* output,
    size_t size) {
  float* carrier = buffer_[0];
  float* carrier_i = &src_buffer_[0][0];
  float* carrier_q = &src_buffer_[0][size];

  // Generate the I/Q components.
  if (parameters_.carrier_shape) {
    float d = parameters_.raw_algorithm_pot - 0.5f;
    float linear_modulation_amount = 1.0f - 14.0f * d * d;
    if (linear_modulation_amount < 0.0f) {
      linear_modulation_amount = 0.0f;
    }
    float frequency = parameters_.raw_algorithm_pot;
    frequency += linear_modulation_amount * parameters_.raw_algorithm_cv;

    float direction = frequency >= 0.5f ? 1.0f : -1.0f;
    frequency = 2.0f * fabs(frequency - 0.5f);
    frequency = frequency <= 0.4f
        ? frequency * frequency * frequency * 62.5f
        : 4.0f * SemitonesToRatio(180.0f * (frequency - 0.4f));
    frequency *= SemitonesToRatio(
        parameters_.raw_algorithm_cv * 60.0f * \
            (1.0f - linear_modulation_amount) * direction);
    frequency *= direction;

    float shape = static_cast<float>(parameters_.carrier_shape - 1) * 0.5f;
    quadrature_oscillator_.Render(shape, frequency, carrier_i, carrier_q, size);
  } else {
    for (size_t i = 0; i < size; ++i) {
      carrier[i] = static_cast<float>(input[i].l) / 32768.0f;
    }
    quadrature_transform_[0].Process(carrier, carrier_i, carrier_q, size);

    ParameterInterpolator phase_shift(
        &previous_parameters_.raw_algorithm,
        parameters_.raw_algorithm,
        size);

    for (size_t i = 0; i < size; ++i) {
      float x_i = carrier_i[i];
      float x_q = carrier_q[i];
      float angle = phase_shift.Next();
      float r_sin = Interpolate(lut_sin, angle, 1024.0f);
      float r_cos = Interpolate(lut_sin + 256, angle, 1024.0f);
      carrier_i[i] = r_sin * x_i + r_cos * x_q;
      carrier_q[i] = r_sin * x_q - r_cos * x_i;
    }
  }

  // Setup parameter interpolation.
  ParameterInterpolator mix(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter,
      size);
  ParameterInterpolator feedback_amount(
      &previous_parameters_.raw_level[0],
      parameters_.raw_level[0],
      size);
  ParameterInterpolator dry_wet(
      &previous_parameters_.raw_level[1],
      parameters_.raw_level[1],
      size);

  float feedback_sample = feedback_sample_;
  for (size_t i = 0; i < size; ++i) {
    float timbre = mix.Next();
    float modulator_i, modulator_q;

    // Start from the signal from input 2, with non-linear gain.
    float in = static_cast<float>(input->r) / 32768.0f;

    if (parameters_.carrier_shape) {
      in += static_cast<float>(input->l) / 32768.0f;
    }

    float modulator = in;

    // Apply feedback if necessary, and soft limit.
    float amount = feedback_amount.Next();
    amount *= (2.0f - amount);
    amount *= (2.0f - amount);

    // mic.w feedback amount tweak.
    float max_fb = 1.0f + 2.0f * (timbre - 0.5f) * (timbre - 0.5f);
    modulator += amount * (
        SoftClip(modulator + max_fb * feedback_sample * amount) - modulator);

    quadrature_transform_[1].Process(modulator, &modulator_i, &modulator_q);

    // Modulate!
    float a = *carrier_i++ * modulator_i;
    float b = *carrier_q++ * modulator_q;
    float up = a - b;
    float down = a + b;
    float lut_index = timbre;
    float fade_in = Interpolate(lut_xfade_in, lut_index, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, lut_index, 256.0f);
    float main = up * fade_in + down * fade_out;
    float aux = down * fade_in + up * fade_out;

    // Simple LP to prevent feedback of high-frequencies.
    ONE_POLE(feedback_sample, main, 0.2f);

    float wet_dry = 1.0f - dry_wet.Next();
    main += wet_dry * (in - main);
    aux += wet_dry * (in - aux);

    output->l = Clip16(static_cast<int32_t>(main * 32768.0f));
    output->r = Clip16(static_cast<int32_t>(aux * 32768.0f));
    ++output;
    ++input;
  }
  feedback_sample_ = feedback_sample;
  previous_parameters_ = parameters_;
}

void Modulator::ProcessMeta(
    ShortFrame* input,
    ShortFrame* output,
    size_t size) {
  float* carrier = buffer_[0];
  float* modulator = buffer_[1];
  float* main_output = buffer_[0];
  float* aux_output = buffer_[2];
  float* oversampled_carrier = src_buffer_[0];
  float* oversampled_modulator = src_buffer_[1];
  float* oversampled_output = src_buffer_[0];

  // 0.0: use cross-modulation algorithms. 1.0f: use vocoder.
  float vocoder_amount = (
      parameters_.modulation_algorithm - 0.7f) * 20.0f + 0.5f;
  CONSTRAIN(vocoder_amount, 0.0f, 1.0f);

  if (!parameters_.carrier_shape) {
    fill(&aux_output[0], &aux_output[size], 0.0f);
  }

  // Convert audio inputs to float and apply VCA/saturation (5.8% per channel)
  short* input_samples = &input->l;
  for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
      amplifier_[i].Process(
          parameters_.channel_drive[i],
          1.0f - vocoder_amount,
          input_samples + i,
          buffer_[i],
          aux_output,
          2,
          size);
  }

  // If necessary, render carrier. Otherwise, sum signals 1 and 2 for aux out.
  if (parameters_.carrier_shape) {
    // Scale phase-modulation input.
    for (size_t i = 0; i < size; ++i) {
      internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f;
    }
    // Xmod: sine, triangle saw.
    // Vocoder: saw, pulse, noise.
    OscillatorShape xmod_shape = static_cast<OscillatorShape>(
        parameters_.carrier_shape - 1);
    OscillatorShape vocoder_shape = static_cast<OscillatorShape>(
        parameters_.carrier_shape + 1);

    // Outside of the transition zone between the cross-modulation and vocoding
    // algorithm, we need to render only one of the two oscillators.
    if (vocoder_amount == 0.0f) {
      xmod_oscillator_.Render(
          xmod_shape,
          parameters_.note,
          internal_modulation_,
          aux_output,
          size);
      for (size_t i = 0; i < size; ++i) {
        carrier[i] = aux_output[i] * kXmodCarrierGain;
      }
    } else if (vocoder_amount >= 0.5f) {
      float carrier_gain = vocoder_oscillator_.Render(
          vocoder_shape,
          parameters_.note,
          internal_modulation_,
          aux_output,
          size);
      for (size_t i = 0; i < size; ++i) {
        carrier[i] = aux_output[i] * carrier_gain;
      }
    } else {
      float balance = vocoder_amount * 2.0f;
      xmod_oscillator_.Render(
          xmod_shape,
          parameters_.note,
          internal_modulation_,
          carrier,
          size);
      float carrier_gain = vocoder_oscillator_.Render(
          vocoder_shape,
          parameters_.note,
          internal_modulation_,
          aux_output,
          size);
      for (size_t i = 0; i < size; ++i) {
        float a = carrier[i];
        float b = aux_output[i];
        aux_output[i] = a + (b - a) * balance;
        a *= kXmodCarrierGain;
        b *= carrier_gain;
        carrier[i] = a + (b - a) * balance;
      }
    }
  }

  if (vocoder_amount < 0.5f) {
    src_up_[0].Process(carrier, oversampled_carrier, size);
    src_up_[1].Process(modulator, oversampled_modulator, size);

    float algorithm = min(parameters_.modulation_algorithm * 8.0f, 5.999f);
    float previous_algorithm = min(
        previous_parameters_.modulation_algorithm * 8.0f, 5.999f);

    MAKE_INTEGRAL_FRACTIONAL(algorithm);
    MAKE_INTEGRAL_FRACTIONAL(previous_algorithm);

    if (algorithm_integral != previous_algorithm_integral) {
      previous_algorithm_fractional = algorithm_fractional;
    }

    (this->*xmod_table_[algorithm_integral])(
        previous_algorithm_fractional,
        algorithm_fractional,
        previous_parameters_.skewed_modulation_parameter(),
        parameters_.skewed_modulation_parameter(),
        oversampled_modulator,
        oversampled_carrier,
        oversampled_output,
        size * kOversampling);

    src_down_.Process(oversampled_output, main_output, size * kOversampling);
  } else {
    float release_time = 4.0f * (parameters_.modulation_algorithm - 0.75f);
    CONSTRAIN(release_time, 0.0f, 1.0f);

    vocoder_.set_release_time(release_time * (2.0f - release_time));
    vocoder_.set_formant_shift(parameters_.modulation_parameter);
    vocoder_.Process(modulator, carrier, main_output, size);
  }

  // Cross-fade to raw modulator for the transition between cross-modulation
  // algorithms and vocoding algorithms.
  float transition_gain = 2.0f * (vocoder_amount < 0.5f
      ? vocoder_amount
      : 1.0f - vocoder_amount);
  if (transition_gain != 0.0f) {
    for (size_t i = 0; i < size; ++i) {
      main_output[i] += transition_gain * (modulator[i] - main_output[i]);
    }
  }

  // Convert back to integer and clip.
  while (size--) {
    output->l = Clip16(static_cast<int32_t>(*main_output * 32768.0f));
    output->r = Clip16(static_cast<int32_t>(*aux_output * 16384.0f));
    ++main_output;
    ++aux_output;
    ++output;
  }
  previous_parameters_ = parameters_;
}

template<XmodAlgorithm algorithm>
void Modulator::Process1(ShortFrame* input, ShortFrame* output, size_t size) {
  float* carrier = buffer_[0];
  float* modulator = buffer_[1];
  float* main_output = buffer_[0];
  float* aux_output = buffer_[2];
  float* oversampled_carrier = src_buffer_[0];
  float* oversampled_modulator = src_buffer_[1];
  float* oversampled_output = src_buffer_[0];

  if (!parameters_.carrier_shape) {
    fill(&aux_output[0], &aux_output[size], 0.0f);
  }

  // Convert audio inputs to float and apply VCA/saturation (5.8% per channel)
  short* input_samples = &input->l;
  for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
      amplifier_[i].Process(
          parameters_.channel_drive[i],
          1.0f,
          input_samples + i,
          buffer_[i],
          aux_output,
          2,
          size);
  }

  // If necessary, render carrier. Otherwise, sum signals 1 and 2 for aux out.
  if (parameters_.carrier_shape) {
    // Scale phase-modulation input.
    for (size_t i = 0; i < size; ++i) {
      internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f;
    }

    OscillatorShape xmod_shape = static_cast<OscillatorShape>(
        parameters_.carrier_shape - 1);
    xmod_oscillator_.Render(
          xmod_shape,
          parameters_.note,
          internal_modulation_,
          aux_output,
          size);
    for (size_t i = 0; i < size; ++i) {
      carrier[i] = aux_output[i] * kXmodCarrierGain;
    }
  }

  src_up2_[0].Process(carrier, oversampled_carrier, size);
  src_up2_[1].Process(modulator, oversampled_modulator, size);

  ProcessXmod<algorithm>(
        previous_parameters_.modulation_algorithm,
        parameters_.modulation_algorithm,
        previous_parameters_.skewed_modulation_parameter(),
        parameters_.skewed_modulation_parameter(),
        oversampled_modulator,
        oversampled_carrier,
        oversampled_output,
        size * kLessOversampling);

  src_down2_[0].Process(oversampled_output, main_output, size * kLessOversampling);

  // Convert back to integer and clip.
  while (size--) {
    output->l = Clip16(static_cast<int32_t>(*main_output * 32768.0f));
    output->r = Clip16(static_cast<int32_t>(*aux_output * 16384.0f));
    ++main_output;
    ++aux_output;
    ++output;
  }
  previous_parameters_ = parameters_;
}

void Modulator::ProcessBitcrusher(ShortFrame* input, ShortFrame* output, size_t size) {
  float* carrier = buffer_[0];
  float* modulator = buffer_[1];
  float* main_output = buffer_[0];
  float* aux_output = buffer_[2];

  if (!parameters_.carrier_shape) {
    fill(&aux_output[0], &aux_output[size], 0.0f);
  }

  // Convert audio inputs to float and apply VCA/saturation (5.8% per channel)
  short* input_samples = &input->l;
  for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
      amplifier_[i].Process(
          parameters_.channel_drive[i],
          1.0f,
          input_samples + i,
          buffer_[i],
          aux_output,
          2,
          size);
  }

  // If necessary, render carrier. Otherwise, sum signals 1 and 2 for aux out.
  if (parameters_.carrier_shape) {
    // Scale phase-modulation input.
    for (size_t i = 0; i < size; ++i) {
      internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f;
    }

    OscillatorShape xmod_shape = static_cast<OscillatorShape>(
        parameters_.carrier_shape - 1);
    xmod_oscillator_.Render(
          xmod_shape,
          parameters_.note,
          internal_modulation_,
          aux_output,
          size);
    for (size_t i = 0; i < size; ++i) {
      carrier[i] = aux_output[i] * kXmodCarrierGain;
    }
  }

  // make sure it dry: parameter doesn't go to 0.0f apparently
  float mod_1 = (parameters_.modulation_parameter - 0.05f) / 0.95f;
  float mod_2 = (previous_parameters_.modulation_parameter - 0.05f) / 0.95f;
  CONSTRAIN(mod_1, 0.0f, 1.0f);
  CONSTRAIN(mod_2, 0.0f, 1.0f);

  ProcessXmod<ALGORITHM_BITCRUSHER>(
        previous_parameters_.modulation_algorithm,
        parameters_.modulation_algorithm,
        mod_1,
        mod_2,
        carrier,
  modulator,
        main_output,
  aux_output,
        size);

  // Convert back to integer and clip.
  while (size--) {
    output->l = Clip16(static_cast<int32_t>(*main_output * 32768.0f));
    output->r = Clip16(static_cast<int32_t>(*aux_output * 16384.0f));
    ++main_output;
    ++aux_output;
    ++output;
  }
  previous_parameters_ = parameters_;

}

void Modulator::ProcessDelay(ShortFrame* input, ShortFrame* output, size_t size) {
  ShortFrame *buffer = delay_buffer_;

  static FloatFrame feedback_sample;

  static int32_t write_head = 0;

  static float write_position = 0.0f;

  static FloatFrame previous_samples[3] = {{0.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f}};

  float time = previous_parameters_.modulation_parameter * (DELAY_SIZE-10) + 5;
  float time_end = parameters_.modulation_parameter * (DELAY_SIZE-10) + 5;
  float time_increment = (time_end - time) / static_cast<float>(size);

  float feedback = previous_parameters_.raw_level[0];
  float feedback_end = parameters_.raw_level[0];
  float feedback_increment = (feedback_end - feedback) / static_cast<float>(size);

  float drywet = previous_parameters_.raw_dry_wet_pot;
  float drywet_end = parameters_.raw_dry_wet_pot;
  float drywet_increment = (drywet_end - drywet) / static_cast<float>(size);

  float rate;
  float rate_end;
  float rate_increment;
  bool freeze_active = parameters_.raw_dry_wet_cv > 0.5f && previous_parameters_.raw_dry_wet_cv > 0.5f;

  if (freeze_active) {
    feedback = 1.0f; // Force feedback to 100%
    feedback_increment = 0.0f;
  }

  bool locked_frequency = parameters_.level_2_cv_patched;

  if(locked_frequency) {
      static const float speeds[] = { 
        -1.0f, -0.75f, -0.5f, -0.25f, -0.125f, -0.0625f, 0.003125f, 0.0625f, 0.125f, 0.25f, 0.5f, 0.75f, 1.0f
      };
      const int num_steps = sizeof(speeds) / sizeof(float);

      int start_index = static_cast<int>(previous_parameters_.raw_algorithm * (num_steps - 1) + 0.5f);
      CONSTRAIN(start_index, 0, num_steps - 1);
      rate = speeds[start_index];

      int end_index = static_cast<int>(parameters_.raw_algorithm * (num_steps - 1) + 0.5f);
      CONSTRAIN(end_index, 0, num_steps - 1);
      rate_end = speeds[end_index];

      rate_increment = (rate_end - rate) / static_cast<float>(size);
  } else {
    rate = previous_parameters_.raw_algorithm;
    rate = rate * 2.0f - 1.0f;
    rate *= rate * rate;

    rate_end = parameters_.raw_algorithm;
    rate_end = rate_end * 2.0f - 1.0f;
    rate_end = rate_end * rate_end * rate_end;
    
    rate_increment = (rate_end - rate) / static_cast<float>(size);
  }

  filter_[0].set_f<stmlib::FREQUENCY_FAST>(0.0008f);
  filter_[1].set_f<stmlib::FREQUENCY_FAST>(0.0008f);

  while (size--) {

    static float lp_time = 0.0f;
    ONE_POLE(lp_time, time, 0.00002f);

    static float lp_rate;
    ONE_POLE(lp_rate, rate, 0.007f);
    float sample_rate = fabsf(lp_rate);
    CONSTRAIN(sample_rate, 0.001f, 1.0f);
    int direction = lp_rate > 0.0f ? 1 : -1;

    FloatFrame in;
    in.l = static_cast<float>(input->l) / 32768.0f;
    in.r = static_cast<float>(input->r) / 32768.0f;

    FloatFrame fb;

    if (parameters_.carrier_shape == 3) {
      // invert feedback channels (ping-pong)
      fb.l = feedback_sample.r * feedback * 1.1f;
      fb.r = feedback_sample.l * feedback * 1.1f;
    } else if (parameters_.carrier_shape == 2) {
      // simulate tape hiss with a bit of noise
      float noise1 = Random::GetFloat();
      float noise2 = Random::GetFloat();
      fb.l = feedback_sample.l + noise1 * 0.002f;
      fb.r = feedback_sample.r + noise2 * 0.002f;
      // apply filters: fixed high-pass and varying low-pass with attenuation
      filter_[2].set_f<stmlib::FREQUENCY_FAST>(feedback / 12.0f);
      filter_[3].set_f<stmlib::FREQUENCY_FAST>(feedback / 12.0f);
      fb.l = filter_[0].Process<stmlib::FILTER_MODE_HIGH_PASS>(fb.l);
      fb.r = filter_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(fb.r);
      fb.l = feedback * (2.0f - feedback) * 1.1f *
        filter_[2].Process<stmlib::FILTER_MODE_LOW_PASS>(fb.l);
      fb.r = feedback * (2.0f - feedback) * 1.1f *
        filter_[3].Process<stmlib::FILTER_MODE_LOW_PASS>(fb.r);
      // apply soft saturation with a bit of bias
      fb.l = SoftLimit(fb.l * 1.4f + 0.1f) / 1.4f - SoftLimit(0.1f);
      fb.r = SoftLimit(fb.r * 1.4f + 0.1f) / 1.4f - SoftLimit(0.1f);
    } else if (parameters_.carrier_shape == 0) {
      // open feedback loop
      fb.l = feedback * 1.1f * in.r;
      fb.r = feedback_sample.l;
      in.r = 0.0f;
    } else {
      // classic dual delay
      fb.l = feedback_sample.l * feedback * 1.1f;
      fb.r = feedback_sample.r * feedback * 1.1f;
    }

    // input + feedback
    FloatFrame mix;

    if (freeze_active) {
      // When frozen, soft-clip the feedback and write only that to the buffer.
      fb.l = stmlib::SoftLimit(fb.l * 0.95f) / 0.95f;
      fb.r = stmlib::SoftLimit(fb.r * 0.95f) / 0.95f;
      mix.l = fb.l;
      mix.r = fb.r;
    } else {
      // Normal operation: mix input with feedback.
      mix.l = in.l + fb.l;
      mix.r = in.r + fb.r;
    }

    // write to buffer
    while (write_position < 1.0f) {

      // read somewhere between the input and the previous input
      FloatFrame s = {0, 0};

      if (delay_interpolation_ == INTERPOLATION_ZOH) {
        s.l = mix.l;
        s.r = mix.r;
      } else if (delay_interpolation_ == INTERPOLATION_LINEAR) {
        s.l = previous_samples[0].l + (mix.l - previous_samples[0].l) * write_position;
        s.r = previous_samples[0].r + (mix.r - previous_samples[0].r) * write_position;
      } else if (delay_interpolation_ == INTERPOLATION_HERMITE) {
        FloatFrame xm1 = previous_samples[2];
        FloatFrame x0 = previous_samples[1];
        FloatFrame x1 = previous_samples[0];
        FloatFrame x2 = mix;

        FloatFrame c = { (x1.l - xm1.l) * 0.5f,
                         (x1.r - xm1.r) * 0.5f };
        FloatFrame v = { (float)(x0.l - x1.l), (float)(x0.r - x1.r)};
        FloatFrame w = { c.l + v.l, c.r + v.r };
        FloatFrame a = { w.l + v.l + (x2.l - x0.l) * 0.5f,
                         w.r + v.r + (x2.r - x0.r) * 0.5f };
        FloatFrame b_neg = { w.l + a.l, w.r + a.r };
        float t = write_position;
        s.l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0.l);
        s.r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0.r);
      }

      // write this to buffer
      buffer[write_head].l = Clip16((s.l) * 32768.0f);
      buffer[write_head].r = Clip16((s.r) * 32768.0f);

      write_position += 1.0f / sample_rate;

      write_head += direction;
      // wraparound
      if (write_head >= DELAY_SIZE)
        write_head -= DELAY_SIZE;
      else if (write_head < 0)
        write_head += DELAY_SIZE;
    }

    write_position--;

    previous_samples[2] = previous_samples[1];
    previous_samples[1] = previous_samples[0];
    previous_samples[0] = mix;

    // read from buffer

    float index = write_head - write_position * sample_rate * direction - lp_time;

    while (index < 0) {
      index += DELAY_SIZE;
    }

    MAKE_INTEGRAL_FRACTIONAL(index);

    ShortFrame xm1 = buffer[index_integral];
    ShortFrame x0 = buffer[(index_integral + 1) % DELAY_SIZE];
    ShortFrame x1 = buffer[(index_integral + 2) % DELAY_SIZE];
    ShortFrame x2 = buffer[(index_integral + 3) % DELAY_SIZE];

    FloatFrame wet;

    if (delay_interpolation_ == INTERPOLATION_ZOH) {
      wet.l = xm1.l;
      wet.r = xm1.r;
    } else if (delay_interpolation_ == INTERPOLATION_LINEAR) {
      wet.l = xm1.l + (x0.l - xm1.l) * index_fractional;
      wet.r = xm1.r + (x0.r - xm1.r) * index_fractional;
    } else if (delay_interpolation_ == INTERPOLATION_HERMITE) {
      FloatFrame c = { (x1.l - xm1.l) * 0.5f,
                       (x1.r - xm1.r) * 0.5f };
      FloatFrame v = { (float)(x0.l - x1.l), (float)(x0.r - x1.r)};
      FloatFrame w = { c.l + v.l, c.r + v.r };
      FloatFrame a = { w.l + v.l + (x2.l - x0.l) * 0.5f,
                       w.r + v.r + (x2.r - x0.r) * 0.5f };
      FloatFrame b_neg = { w.l + a.l, w.r + a.r };
      float t = index_fractional;
      wet.l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0.l);
      wet.r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0.r);
    }

    wet.l /= 32768.0f;
    wet.r /= 32768.0f;

    if (!freeze_active) {
      // attenuate output at low sample rate to mask stupid
      // discontinuity bug
      float gain = sample_rate / 0.01f;
      CONSTRAIN(gain, 0.0f, 1.0f);
      wet.l *= gain * gain;
      wet.r *= gain * gain;
    }

    feedback_sample = wet;

    float fade_in = Interpolate(lut_xfade_in, drywet, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, drywet, 256.0f);

    if (parameters_.carrier_shape == 0) {
      // if open feedback loop, AUX is the wet signal and OUT
      // crossfades between inputs
      in.r = static_cast<float>(input->r) / 32768.0f;
      output->l = Clip16((fade_out * in.l + fade_in * in.r) * 32768.0f);
      output->r = Clip16(wet.r * 32768.0f);
    } else if (parameters_.carrier_shape == 2) {
      // analog mode -> soft-clipping
      output->l = SoftConvert((fade_out * in.l + fade_in * wet.l) * 2.0f);
      output->r = SoftConvert((fade_out * in.r + fade_in * wet.r) * 2.0f);
    } else {
      output->l = Clip16((fade_out * in.l + fade_in * wet.l) * 32768.0f);
      output->r = Clip16((fade_out * in.r + fade_in * wet.r) * 32768.0f);
    }

    feedback += feedback_increment;
    rate += rate_increment;
    time += time_increment;
    drywet += drywet_increment;
    input++;
    output++;
  }

  previous_parameters_ = parameters_;
}


void Modulator::Process(ShortFrame* input, ShortFrame* output, size_t size) {
  if (bypass_) {
    copy(&input[0], &input[size], &output[0]);
    return;
  }

  switch (feature_mode_) {

  case FEATURE_MODE_CRUSH_MIXER:
    ProcessCrushMixer(input, output, size);
    break;

  case FEATURE_MODE_CASSETTE_MIXER:
    ProcessCassetteMixer(input, output, size);
    break;

  case FEATURE_MODE_LOSSY_MIXER:
    ProcessLossyMixer(input, output, size);
    break;

  case FEATURE_MODE_RADIO_MIXER:
    ProcessRadioMixer(input, output, size);
    break;

  case FEATURE_MODE_DREAMY_MIXER:
    ProcessDreamyMixer(input, output, size);
    break;

  case FEATURE_MODE_FREQUENCY_SHIFTER:
    ProcessFreqShifter(input, output, size);
    break;

  case FEATURE_MODE_BITCRUSHER:
    ProcessBitcrusher(input, output, size);
    break;

  case FEATURE_MODE_DELAY:
    ProcessDelay(input, output, size);
    break;

  case FEATURE_MODE_META:
    ProcessMeta(input, output, size);
    break;
  }
}

inline float smoothstep(float edge0, float edge1, float x) {
    x = (x - edge0) / (edge1 - edge0);
    if (x < 0.0f) x = 0.0f;
    else if (x > 1.0f) x = 1.0f;
    return x * x * (3.0f - 2.0f * x);
}

/* static */
inline float Modulator::Diode(float x) {
  // Approximation of diode non-linearity from:
  // Julian Parker - "A simple Digital model of the diode-based ring-modulator."
  // Proc. DAFx-11
  float sign = x > 0.0f ? 1.0f : -1.0f;
  float dead_zone = fabs(x) - 0.667f;
  dead_zone += fabs(dead_zone);
  dead_zone *= dead_zone;
  return 0.04324765822726063f * dead_zone * sign;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_XFADE>(
    float x_1, float x_2, float parameter) {
  float fade_in = Interpolate(lut_xfade_in, parameter, 256.0f);
  float fade_out = Interpolate(lut_xfade_out, parameter, 256.0f);
  return x_1 * fade_in + x_2 * fade_out;
}

template<>
inline float Modulator::Mod<ALGORITHM_CHEBYSCHEV>(
    float x, float p) {

  const float att = 0.01f;
  const float rel = 0.000005f;

  static float envelope_;


  SLOPE(envelope_, fabs(x), att, rel);
  float amp = 0.9f / envelope_;

  const float degree = 6.0f;

  x *= amp;
  float n = p * degree;
  float tn1 = x;
  float tn = 2.0f * x * x - 1;
  while (n > 1.0) {
    float temp = tn;
    tn = 2.0f * x * tn - tn1;
    tn1 = temp;
    n--;
  }

  return (tn1 + (tn - tn1) * n) / amp;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_FOLD>(
    float x_1, float x_2, float parameter) {
  float sum = 0.0f;
  sum += x_1;
  sum += x_2;
  sum += x_1 * x_2 * 0.25f;
  sum *= 0.02f + parameter;
  const float kScale = 2048.0f / ((1.0f + 1.0f + 0.25f) * 1.02f);
  return Interpolate(lut_bipolar_fold + 2048, sum, kScale) * -0.8f;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_FOLD>(
    float x_1, float x_2, float p_1, float p_2) {
  float sum = 0.0f;
  sum += x_1;
  sum += x_2;
  sum += x_1 * x_2 * 0.25f;
  sum *= 0.02f + p_1;
  sum += p_2;
  const float kScale = 2048.0f / ((1.0f + 1.0f + 0.25f) * 1.02f);
  return Interpolate(lut_bipolar_fold + 2048, sum, kScale);
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_ANALOG_RING_MODULATION>(
    float modulator, float carrier, float parameter) {
  carrier *= 2.0f;
  float ring = Diode(modulator + carrier) + Diode(modulator - carrier);
  ring *= (4.0f + parameter * 24.0f);
  return SoftLimit(ring);
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_DIGITAL_RING_MODULATION>(
    float x_1, float x_2, float parameter) {
  float ring = 4.0f * x_1 * x_2 * (1.0f + parameter * 8.0f);
  return ring / (1.0f + fabs(ring));
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_RING_MODULATION>(
    float x_1, float x_2, float p_1, float p_2) {
  float y_1 = Xmod<ALGORITHM_ANALOG_RING_MODULATION>(x_1, x_2, p_2);
  float y_2 = Xmod<ALGORITHM_DIGITAL_RING_MODULATION>(x_1, x_2, p_2);
  return y_2 + (y_1 - y_2) * p_1;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_XOR>(
    float x_1, float x_2, float parameter) {
  short x_1_short = Clip16(static_cast<int32_t>(x_1 * 32768.0f));
  short x_2_short = Clip16(static_cast<int32_t>(x_2 * 32768.0f));
  float mod = static_cast<float>(x_1_short ^ x_2_short) / 32768.0f;
  float sum = (x_1 + x_2) * 0.7f;
  return sum + (mod - sum) * parameter;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_BITCRUSHER>(
    float x_1, float x_2, float p_1, float p_2, float *y_2) {
  short x_1_short = Clip16(static_cast<int32_t>(x_1 * 32768.0f));
  short x_2_short = Clip16(static_cast<int32_t>(x_2 * 32768.0f));

  const float steps = 37.0f;
  float z = p_1 * p_1 * steps;
  MAKE_INTEGRAL_FRACTIONAL(z);

  short z_short_1 = Clip16(static_cast<int32_t>(z_integral/steps * 32768.0f));
  short z_short_2 = Clip16(static_cast<int32_t>((z_integral + 1.0f)/steps * 32768.0f));

  short x_1_mod_1 = x_1_short | z_short_1;
  short x_1_mod_2 = x_1_short | z_short_2;
  short x_1_mod = x_1_mod_1 + (x_1_mod_2 - x_1_mod_1) * z_fractional;

  short x_2_mod_1 = x_2_short | z_short_1;
  short x_2_mod_2 = x_2_short | z_short_2;
  short x_2_mod = x_2_mod_1 + (x_2_mod_2 - x_2_mod_1) * z_fractional;

  *y_2 = static_cast<float>(x_1_mod) / 32768.0f;

  float ops[4];
  ops[0] = static_cast<float>(x_1_mod + x_2_mod) / 32768.0f;
  ops[1] = static_cast<float>(x_1_mod | x_2_mod) / 32768.0f;
  ops[2] = static_cast<float>(x_1_mod ^ x_2_mod) / 32768.0f;
  ops[3] = static_cast<float>(x_1_mod << (x_2_mod >> 12)) / 32768.0f;
  return Interpolate(ops, p_2, 3.0f);
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_COMPARATOR>(
    float modulator, float carrier, float parameter) {
  float x = parameter * 2.995f;
  MAKE_INTEGRAL_FRACTIONAL(x)

  float direct = modulator < carrier ? modulator : carrier;
  float window = fabs(modulator) > fabs(carrier) ? modulator : carrier;
  float window_2 = fabs(modulator) > fabs(carrier)
      ? fabs(modulator)
      : -fabs(carrier);
  float threshold = carrier > 0.05f ? carrier : modulator;

  float sequence[4] = { direct, threshold, window, window_2 };
  float a = sequence[x_integral];
  float b = sequence[x_integral + 1];

  return a + (b - a) * x_fractional;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_COMPARATOR8>(
    float modulator, float carrier, float parameter) {
  float x = parameter * 6.995f;
  MAKE_INTEGRAL_FRACTIONAL(x);
  float y_1, y_2;

  if (x_integral == 0) {
    y_1 = modulator + carrier;
    y_2 = modulator < carrier ? modulator : carrier;
  } else if (x_integral == 1) {
    y_1 = modulator < carrier ? modulator : carrier;
    y_2 = (modulator < carrier ? fabs(carrier) : fabs(modulator)) * 2.0f - 1.0f;
  } else if (x_integral == 2) {
    y_1 = (modulator < carrier ? fabs(carrier) : fabs(modulator)) * 2.0f - 1.0f;
    y_2 = modulator < carrier ? -carrier : modulator;
  } else if (x_integral == 3) {
    y_1 = modulator < carrier ? -carrier : modulator;
    y_2 = fabs(modulator) > fabs(carrier) ? modulator : carrier;
  } else if (x_integral == 4) {
    y_1 = fabs(modulator) > fabs(carrier) ? modulator : carrier;
    y_2 = fabs(modulator) > fabs(carrier)
      ? fabs(modulator)
      : -fabs(carrier);
  } else if (x_integral == 5) {
    y_1 = fabs(modulator) > fabs(carrier)
      ? fabs(modulator)
      : -fabs(carrier);
    y_2 = carrier > 0.05f ? carrier : modulator;
  } else {
    y_1 = carrier > 0.05f ? carrier : modulator;
    y_2 = carrier > 0.05f ? carrier : -fabs(modulator);
  }

  return y_1 + (y_2 - y_1) * x_fractional;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_COMPARATOR_CHEBYSCHEV>(
    float x_1, float x_2, float p_1, float p_2) {

  float x = Xmod<ALGORITHM_COMPARATOR8>(x_1, x_2, p_1);
  x = Mod<ALGORITHM_CHEBYSCHEV>(x, p_2);
  return 0.8f * x;
}

template<>
inline float Modulator::Xmod<ALGORITHM_SIMPLE_BITCRUSHER>(
    float x_1, float x_2, float p_1, float p_2) {
      
  static float phase_a = 0.0f;
  static float phase_b = 0.0f;
  static float s_a = 0.0f;
  static float s_b = 0.0f;
  
  float rate_start = 0.15f;
  float rate_end = 0.00008f;
  float base_rate = rate_start + (rate_end - rate_start) * p_2;
  
  if (p_1 > 0.5f) {
    // We give Channel B a slightly different rate (98% of the main rate)
    // This will make the clicks drift and create a stereo effect.
    float rate_b = base_rate * 0.98f; 
    
    phase_b += rate_b;
    if (phase_b >= 1.0f) {
      phase_b -= 1.0f;
      s_b = x_1;
    }
    return s_b;

  } else {
    phase_a += base_rate;
    if (phase_a >= 1.0f) {
      phase_a -= 1.0f;
      s_a = x_1;
    }
    return s_a;
  }
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_CHEBYSCHEV>(
    float x_1, float x_2, float p_1, float p_2) {

  float x = x_1 + x_2;

  const float degree = 16.0f;

  x *= p_2 * 2.0f;

  if (x < -1.0f) x = -1.0f;
  else if (x > 1.0f) x = 1.0f;

  float n = p_1 * degree;

  float tn1 = x;
  float tn = 2.0f * x * x - 1;
  while (n > 1.0f) {
    float temp = tn;
    tn = 2.0f * x * tn - tn1;
    tn1 = temp;
    n--;
  }

  x = tn1 + (tn - tn1) * n;
  x /= p_2;
  x *= 0.5f;

  return x;
}

/* static */
template<>
inline float Modulator::Xmod<ALGORITHM_NOP>(
    float modulator, float carrier, float parameter) {
  return modulator;
}

// --- Shared mixer helpers ---

inline void Modulator::MixerPrepareInputs(ShortFrame* input, size_t size) {
  float* aux = buffer_[2];
  short* input_samples = &input->l;
  for (int32_t i = 0; i < 2; ++i) {
    amplifier_[i].Process(
        parameters_.channel_drive[i], 1.0f,
        input_samples + i, buffer_[i], aux, 2, size);
  }
}

inline MixerFrame Modulator::MixerComputeFrame(
    float x_1, float x_2, float crossfade, float algo) {
  MixerFrame f;
  f.algo = algo;
  f.crossfade = crossfade;
  f.effect_amount = fabs(algo);
  float fade_in = Interpolate(lut_xfade_in, crossfade, 256.0f);
  float fade_out = Interpolate(lut_xfade_out, crossfade, 256.0f);
  f.mix_A = x_1 * fade_out + x_2 * fade_in;
  f.mix_B = x_1 * fade_in + x_2 * fade_out;
  const float xfade_threshold = 0.05f;
  f.xfade_amount = f.effect_amount < xfade_threshold
      ? f.effect_amount / xfade_threshold : 1.0f;
  return f;
}

inline void Modulator::MixerWriteOutput(
    ShortFrame* output, const MixerFrame& f,
    float processed_A, float processed_B) {
  float fade_wet = Interpolate(lut_xfade_in, f.xfade_amount, 256.0f);
  float fade_dry = Interpolate(lut_xfade_out, f.xfade_amount, 256.0f);
  output->l = Clip16((f.mix_A * fade_dry + processed_A * fade_wet) * 32768.0f);
  output->r = Clip16((f.mix_B * fade_dry + processed_B * fade_wet) * 32768.0f);
}

inline void Modulator::MixerFinalize() {
  previous_parameters_.raw_algorithm = parameters_.raw_algorithm;
  previous_parameters_.modulation_parameter = parameters_.modulation_parameter;
}

// --- Mixer modes ---
void Modulator::ProcessCrushMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  MixerPrepareInputs(input, size);
  int32_t quality_mode = parameters_.carrier_shape;

  float algo_start = SkewRawAlgorithm(previous_parameters_.raw_algorithm);
  float algo_end = SkewRawAlgorithm(parameters_.raw_algorithm);
  ParameterInterpolator algo_interp(&algo_start, algo_end, size);
  ParameterInterpolator xfade_interp(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter, size);

  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];

  while (size--) {
    MixerFrame f = MixerComputeFrame(
        *in_1++, *in_2++, xfade_interp.Next(), algo_interp.Next());

    float processed_A = 0.0f;
    float processed_B = 0.0f;

    if (f.algo < 0.0f) {
      // --- CCW: SRR + Saturation ---
      float aliased_A = Xmod<ALGORITHM_SIMPLE_BITCRUSHER>(f.mix_A, 0.0f, 0.0f, f.effect_amount);
      float aliased_B = Xmod<ALGORITHM_SIMPLE_BITCRUSHER>(f.mix_B, 0.0f, 1.0f, f.effect_amount);

      float drive = 1.0f + f.effect_amount * 2.0f;
      if (quality_mode == 1) {
          // Even harmonics (tube) - asymmetric
          processed_A = stmlib::SoftLimit(aliased_A * drive + 0.2f);
          processed_B = stmlib::SoftLimit(aliased_B * drive + 0.2f);
      } else if (quality_mode == 2) {
          // Hard clip (transistor)
          float a = aliased_A * drive;
          float b = aliased_B * drive;
          if (a > 1.0f) a = 1.0f; else if (a < -1.0f) a = -1.0f;
          if (b > 1.0f) b = 1.0f; else if (b < -1.0f) b = -1.0f;
          processed_A = a;
          processed_B = b;
      } else if (quality_mode == 3) {
          // Digital (quantized)
          float a = stmlib::SoftLimit(aliased_A * drive);
          float b = stmlib::SoftLimit(aliased_B * drive);
          const float levels = 32.0f;
          processed_A = floorf(a * levels) / levels;
          processed_B = floorf(b * levels) / levels;
      } else {
          // Odd harmonics (default symmetric)
          processed_A = stmlib::SoftLimit(aliased_A * drive);
          processed_B = stmlib::SoftLimit(aliased_B * drive);
      }
    } else {
      // --- CW: Chebyshev polynomial waveshaping ---
      float degree_scale = 0.25f + quality_mode * 0.25f;
      float cheby_param = f.effect_amount * degree_scale;
      float comp_param = f.effect_amount;
      processed_A = Xmod<ALGORITHM_COMPARATOR_CHEBYSCHEV>(f.mix_A, f.mix_B, comp_param, cheby_param);
      float cheby_param_r = cheby_param * 1.03f;
      if (cheby_param_r > 1.0f) cheby_param_r = 1.0f;
      processed_B = Xmod<ALGORITHM_COMPARATOR_CHEBYSCHEV>(f.mix_B, f.mix_A, comp_param, cheby_param_r);
    }

    MixerWriteOutput(output++, f, processed_A, processed_B);
  }
  MixerFinalize();
}

void Modulator::ProcessCassetteMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  MixerPrepareInputs(input, size);

  ParameterInterpolator xfade_interp(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter, size);

  // carrier_shape: 0=off(best), 1=green, 2=yellow, 3=red(most lofi)
  int quality = 3 - parameters_.carrier_shape;  // 3=best, 0=lofi
  float degradation = 1.0f - (quality / 3.0f);  // 0.0=pristine, 1.0=trashed

  float algo = SkewRawAlgorithm(parameters_.raw_algorithm);
  float effect_amount = fabs(algo);
  float effect_amount_sqrtf = sqrtf(effect_amount);

  if (algo < 0.0f) {
    // --- SET CASSETTE FILTERS ---
    filter_[4].set_f<stmlib::FREQUENCY_FAST>(0.4f);
    filter_[5].set_f<stmlib::FREQUENCY_FAST>(0.4f);
    // filter_[2].set_f<stmlib::FREQUENCY_FAST>(0.25f); // Hiss filter L // temporarly disabled
    filter_[3].set_f<stmlib::FREQUENCY_DIRTY>(0.25f); // Hiss filter R // try dirty filter instead of fast
    // Quality drives tape bandwidth, knob adds subtle darkening
    float target_filter_cutoff = 0.05f + (1.0f - degradation) * 0.4f - effect_amount * 0.05f;
    tape_lp_l_.set_f<stmlib::FREQUENCY_FAST>(target_filter_cutoff);
    tape_lp_r_.set_f<stmlib::FREQUENCY_FAST>(target_filter_cutoff);

    // Head bump: resonant bass boost that increases with wear
    float head_bump_freq = (80.0f + degradation * 40.0f) * kSamplePeriod;  // 80-120Hz
    float head_bump_q = 1.0f + degradation * 4.0f;  // q3=1(flat), q0=5(resonant)
    lossy_bpf_l_.set_f_q<stmlib::FREQUENCY_FAST>(head_bump_freq, head_bump_q);
    lossy_bpf_r_.set_f_q<stmlib::FREQUENCY_FAST>(head_bump_freq, head_bump_q);
  } else {
    // --- SET VHS FILTERS ---
    //tilt filters
    filter_[0].set_f<stmlib::FREQUENCY_FAST>(0.02f); // L-LPF
    filter_[1].set_f<stmlib::FREQUENCY_FAST>(0.02f); // L-HPF
    filter_[2].set_f<stmlib::FREQUENCY_FAST>(0.02f); // R-LPF
    filter_[3].set_f<stmlib::FREQUENCY_FAST>(0.02f); // R-HPF

    //snow filters (quality drives band positions)
    float wear_mid = degradation * 0.7f;
    float wear_heavy = degradation * 0.4f;
    float low_cutoff  = 3000.0f + 2000.0f * wear_mid;
    float high_cutoff = 4000.0f + 4000.0f * wear_heavy;

    // Try frequency dirty instead of fast
    filter_[4].set_f<stmlib::FREQUENCY_DIRTY>(low_cutoff * kSamplePeriod);  // L-LPF
    filter_[5].set_f<stmlib::FREQUENCY_DIRTY>(high_cutoff * kSamplePeriod); // L-HPF
    filter_[6].set_f<stmlib::FREQUENCY_DIRTY>(low_cutoff * kSamplePeriod);  // R-LPF
    filter_[7].set_f<stmlib::FREQUENCY_DIRTY>(high_cutoff * kSamplePeriod); // R-HPF

    // hiss filters
    tape_lp_l_.set_f<stmlib::FREQUENCY_DIRTY>(0.05f);
    tape_lp_r_.set_f<stmlib::FREQUENCY_DIRTY>(0.25f);

    // Pre-compute VHS dropout params (loop-invariant)
    vhs_age_mid_ = smoothstep(0.3f, 0.8f, effect_amount);
    vhs_age_chewed_ = smoothstep(0.7f, 1.0f, effect_amount);
    float dropout_scale = 1.0f + degradation * 3.0f;
    if (effect_amount < 0.1f) {
        vhs_dropout_base_chance_ = 0.01f * dropout_scale;
        vhs_dropout_max_length_ = 0.010f * dropout_scale;
        vhs_dropout_gain_min_ = 0.6f;
    } else if (effect_amount < 0.6f) {
        vhs_dropout_base_chance_ = 0.05f * dropout_scale;
        vhs_dropout_max_length_ = 0.035f * dropout_scale;
        vhs_dropout_gain_min_ = 0.4f - degradation * 0.2f;
    } else {
        vhs_dropout_base_chance_ = 0.2f * dropout_scale;
        vhs_dropout_max_length_ = 0.070f * dropout_scale;
        vhs_dropout_gain_min_ = 0.2f - degradation * 0.15f;
    }
  }

  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];

  while (size--) {
    MixerFrame f = MixerComputeFrame(
        *in_1++, *in_2++, xfade_interp.Next(), algo);
    float mix_A = f.mix_A;
    float mix_B = f.mix_B;

    float processed_A = 0.0f;
    float processed_B = 0.0f;

    // Hiss noise generator for both modes
    static uint32_t hiss_rng_state_ = 0;
    hiss_rng_state_ = 1664525L * hiss_rng_state_ + 1013904223L;

    if (algo < 0.0f) {
      // --- CCW: Cassette Emulation ---
      // static float random_drift_lfo_phase_ = 0.0f;
      static int random_drift_counter_ = 0;

      static float flutter_lfo_phase = 0.0f;
      static float wow_lfo_phase = 0.0f;
      static float random_drift_target_val_ = 0.5f;
      static float random_drift_current_val_ = 0.5f;
      static float hiss_envelope_ = 0.0f;
      static float allpass_z1_ = 0.0f;
      static float mangle_read_pos_f = 0.0f;
      static float smoothed_read_speed = 1.0f;

      const float mangle_threshold = 0.86f;

      // --- WOW & FLUTTER MODULATION ---

      const float wow_min = 0.5f;
      const float wow_max = 2.5f;
      const float flutter_min = 3.0f;
      const float flutter_max = 7.0f;
      const float flutter_start_point = 0.3f;
      const int lut_size = 1024;

      // 1. Compute frequencies
      float wow_freq = wow_min + effect_amount * (wow_max - wow_min);
      float flutter_freq = flutter_min + effect_amount * (flutter_max - flutter_min);

      // 2. Update LFO phases (fast wrap)
      wow_lfo_phase += wow_freq * kSamplePeriod;
      if (wow_lfo_phase >= 1.0f) wow_lfo_phase -= 1.0f;

      flutter_lfo_phase += flutter_freq * kSamplePeriod;
      if (flutter_lfo_phase >= 1.0f) flutter_lfo_phase -= 1.0f;

      // 4. Determine flutter scaling (fade in)
      float flutter_scale = 0.0f;
      if (effect_amount > flutter_start_point) {
          flutter_scale = (effect_amount - flutter_start_point) / (mangle_threshold - flutter_start_point);
          if (flutter_scale < 0.0f) flutter_scale = 0.0f;
          if (flutter_scale > 1.0f) flutter_scale = 1.0f;
      }

      // 5. Lookup wow (stereo offset +1/8 cycle)
      float wow_index = wow_lfo_phase * lut_size;
      int wow_i = (int)wow_index;
      float wow_frac = wow_index - wow_i;

      float wow_l = lut_sin[wow_i];
      float wow_next = lut_sin[(wow_i + 1) & (lut_size - 1)];
      float wow_mod = wow_l + (wow_next - wow_l) * wow_frac;

      // 2nd harmonic for mechanical capstan lurch (worn tape wobbles asymmetrically)
      int wow_2nd_i = (wow_i * 2) & (lut_size - 1);
      wow_mod += lut_sin[wow_2nd_i] * degradation * 0.3f;

      int wow_i_r = (wow_i + (lut_size >> 3)) & (lut_size - 1);
      float wow_mod_r = lut_sin[wow_i_r];
      int wow_2nd_i_r = (wow_i_r * 2) & (lut_size - 1);
      wow_mod_r += lut_sin[wow_2nd_i_r] * degradation * 0.3f;

      // 6. Lookup flutter (stereo offset +1/4 cycle)
      float flutter_index = flutter_lfo_phase * lut_size;
      int flutter_i = (int)flutter_index;
      float flutter_frac = flutter_index - flutter_i;

      float flutter_l = lut_sin[flutter_i];
      float flutter_next = lut_sin[(flutter_i + 1) & (lut_size - 1)];
      float flutter_mod = flutter_l + (flutter_next - flutter_l) * flutter_frac;

      int flutter_i_r = (flutter_i + (lut_size >> 2)) & (lut_size - 1);
      float flutter_mod_r = lut_sin[flutter_i_r];

      // 7. Combine modulations, applying flutter scale
      float wow_flutter_scale = 1.0f + degradation * 1.5f;  // 1x to 2.5x depth
      float total_mod_l = (wow_mod + flutter_mod * flutter_scale) * 0.5f * effect_amount_sqrtf * wow_flutter_scale;
      float total_mod_r = (wow_mod_r + flutter_mod_r * flutter_scale) * 0.5f * effect_amount_sqrtf * wow_flutter_scale;

      // 8. Compute final delay offsets
      const float kFixedTapeDelay = 1.0f;
      float depth_multiplier = 3.0f + effect_amount * 4.0f;  // 3.0 at knob=0, ~6.4 at knob=0.86
      float delay_offset_l = total_mod_l * depth_multiplier;
      float delay_offset_r = total_mod_r * depth_multiplier;

      float base_read_pos = shared_write_pos_ - kFixedTapeDelay;

      if (effect_amount >= mangle_threshold) {
          // --- "TAPE MANGLE" (PITCH SHIFT) ---
          float mangle_amount = (effect_amount - mangle_threshold) / (1.0f - mangle_threshold);

          // Hard step: half-speed, then quarter-speed at 80%
          float target_speed = (mangle_amount < 0.8f) ? 0.5f : 0.25f;

          // Smooth the speed (~10ms ramp) — prevents click on mangle entry
          ONE_POLE(smoothed_read_speed, target_speed, 0.005f);

          mangle_read_pos_f += smoothed_read_speed;
          if (mangle_read_pos_f >= (float)kSharedDelaySize)
              mangle_read_pos_f -= (float)kSharedDelaySize;

          // Proper circular distance (how far read is behind write)
          float distance = (float)shared_write_pos_ - mangle_read_pos_f;
          if (distance < 0.0f) distance += (float)kSharedDelaySize;

          // Soft pull-forward when read gets too close to write
          const float kSafeZone = 128.0f;
          const float kTargetDistance = 512.0f;
          if (distance < kSafeZone || distance > (float)kSharedDelaySize - kSafeZone) {
              // Pull toward safe target — brief pitch-up instead of click
              float target_pos = (float)shared_write_pos_ - kTargetDistance;
              if (target_pos < 0.0f) target_pos += (float)kSharedDelaySize;

              float diff = target_pos - mangle_read_pos_f;
              if (diff > (float)kSharedDelaySize * 0.5f) diff -= (float)kSharedDelaySize;
              if (diff < -(float)kSharedDelaySize * 0.5f) diff += (float)kSharedDelaySize;
              mangle_read_pos_f += diff * 0.02f;

              if (mangle_read_pos_f < 0.0f) mangle_read_pos_f += (float)kSharedDelaySize;
              if (mangle_read_pos_f >= (float)kSharedDelaySize) mangle_read_pos_f -= (float)kSharedDelaySize;
          }

          base_read_pos = mangle_read_pos_f;
      } else {
          // --- ORIGINAL WOW/FLUTTER ---
          base_read_pos = shared_write_pos_ - kFixedTapeDelay;
          mangle_read_pos_f = base_read_pos;
          // Ramp speed back toward 1.0 for clean re-entry
          ONE_POLE(smoothed_read_speed, 1.0f, 0.01f);
      }

      float read_pos_f_L = base_read_pos + delay_offset_l;
      float read_pos_f_R = base_read_pos + delay_offset_r;
      
      // --- Slow Random LFO (for Fake Stereo) ---
      // Run this once per block or every few hundred samples
      if (--random_drift_counter_ <= 0) {
          random_drift_counter_ = static_cast<int>(96000.0f / 0.3f); // ~3.3s at 96kHz
          // random_drift_target_val_ = Random::GetFloat();
          //reuse hiss_rng_state_ for random number
          random_drift_target_val_ = static_cast<float>((hiss_rng_state_ & 0x7fffffff) / 2147483647.0f);
      }

      // Per-sample smoothing (still cheap)
      random_drift_current_val_ += 0.00005f * (random_drift_target_val_ - random_drift_current_val_);
      float slow_random_drift = (random_drift_current_val_ - 0.5f) * 0.2f;
      
      // --- Wrap read position ---
      //replace read position fmod with manual wrap
      if (read_pos_f_L < 0.0f) {
          read_pos_f_L += (float)kSharedDelaySize;
      } else if (read_pos_f_L >= (float)kSharedDelaySize) {
          read_pos_f_L -= (float)kSharedDelaySize;
      }
      MAKE_INTEGRAL_FRACTIONAL(read_pos_f_L);

      float delayed_l;
      {
          int32_t index_integral = read_pos_f_L_integral;
          float t = read_pos_f_L_fractional;

          int32_t i1_l = index_integral + 1; if (i1_l >= kSharedDelaySize) i1_l = 0;
          float x0 = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
          float x1 = static_cast<float>(delay_buffer_[i1_l].l) * kToFloat;
          delayed_l = x0 + (x1 - x0) * t;
      }

      //replace read position fmod with manual wrap
      if (read_pos_f_R < 0.0f) {
        read_pos_f_R += (float)kSharedDelaySize;
      } else if (read_pos_f_R >= (float)kSharedDelaySize) {
        read_pos_f_R -= (float)kSharedDelaySize;
      }
      MAKE_INTEGRAL_FRACTIONAL(read_pos_f_R);

      float delayed_r;
      {
          int32_t index_integral = read_pos_f_R_integral;
          float t = read_pos_f_R_fractional;
          int32_t i1_r = index_integral + 1; if (i1_r >= kSharedDelaySize) i1_r = 0;
          float x0 = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
          float x1 = static_cast<float>(delay_buffer_[i1_r].r) * kToFloat;
          delayed_r = x0 + (x1 - x0) * t;
      }

      // --- FAKE STEREO ---
      {
          float coeff = 0.4f + slow_random_drift * 1.0f;
          float x_n = delayed_r;
          float w = x_n + coeff * allpass_z1_;
          float y_n = -coeff * w + allpass_z1_;

          allpass_z1_ = w;
          delayed_r = y_n;
      }
      
      // --- Envelope Follower ---
      float input_level = (fabs(delayed_l) + fabs(delayed_r)) * 0.5f;
      ONE_POLE(hiss_envelope_, input_level, 0.0002f); 

      // --- Saturation (quality drives it, knob adds subtle extra) ---
      float drive = 1.0f + degradation * 4.0f + effect_amount * 1.0f;
      float offset = degradation * 0.2f + effect_amount * 0.05f;
      // Cubic soft-clip (division-free, saves ~24 cyc vs SoftLimit)
      float s_l = (delayed_l * drive) + offset;
      if (s_l > 1.0f) s_l = 1.0f;
      else if (s_l < -1.0f) s_l = -1.0f;
      else s_l = s_l * (1.5f - 0.5f * s_l * s_l);
      float sat_l = s_l;

      float s_r = (delayed_r * drive) + offset;
      if (s_r > 1.0f) s_r = 1.0f;
      else if (s_r < -1.0f) s_r = -1.0f;
      else s_r = s_r * (1.5f - 0.5f * s_r * s_r);
      float sat_r = s_r;

      // --- Head Bump (worn playback head resonance, mono — saves 1 SVF) ---
      {
          float head_bump_amount = degradation * 0.3f;  // 0 at q3, 0.3 at q0
          float bump = lossy_bpf_l_.Process<stmlib::FILTER_MODE_BAND_PASS>(sat_l);
          sat_l += bump * head_bump_amount;
          sat_r += bump * head_bump_amount;
      }

      // --- De-emphasis ---
      sat_l = filter_[4].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_l);
      sat_r = filter_[5].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_r);

      // --- Filtering (Tape Bandwidth) ---
      float filtered_l = filter_[6].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_l);
      float filtered_r = filter_[7].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_r);

      // --- Hiss (quality drives level, knob adds subtle modulation) ---
      float hiss_degradation = degradation * degradation;  // q0=1.0, q1=0.44, q2=0.11, q3=0
      float hiss_base = hiss_degradation * 0.12f;  // constant bias noise floor
      float hiss_dynamic = hiss_envelope_ * hiss_degradation * 0.4f;
      float hiss_knob_mod = effect_amount * 0.02f;
      float hiss_level = hiss_base + hiss_dynamic + hiss_knob_mod;

      float hiss_raw_l = static_cast<float>(hiss_rng_state_) / 4294967296.0f;
      float hiss_l = (hiss_raw_l - 0.5f) * hiss_level;

      float hiss_filtered_l = filter_[3].Process<stmlib::FILTER_MODE_HIGH_PASS>(hiss_l);
      // float hiss_filtered_r = filter_[2].Process<stmlib::FILTER_MODE_HIGH_PASS>(hiss_r); 

      processed_A = filtered_l + hiss_filtered_l;
      processed_B = filtered_r - hiss_filtered_l;
    } else {
      // --- CW: VHS Emulation ---
      // dropout params pre-computed before the loop
      float age_chewed = vhs_age_chewed_;
      float dropout_base_chance = vhs_dropout_base_chance_;
      float dropout_max_length = vhs_dropout_max_length_;
      float dropout_gain_min = vhs_dropout_gain_min_;
      
      // --- State Variables ---
      static float dropout_check_timer = 0.1f;
      static float dropout_duration_timer = 0.0f;
      static float dropout_envelope = 1.0f;
      static float tracking_lfo_phase = 0.0f;
      static float hum_lfo_phase_ = 0.0f;
      static float prev_hum_phase = 0.0f;
      static float click_envelope = 0.0f;
      static float tilt_envelope = 0.0f;
      
      // --- Noise Signal (Band-passed) ---
      float input_level = (fabs(mix_A) + fabs(mix_B)) * 0.5f;
      static float noise_agc = 0.2f;
      ONE_POLE(noise_agc, input_level, 0.0005f);

      // “Breathing” makes noise rise when input is quiet
      float breathing = (1.0f - noise_agc);

      // Raw mid-band noise source
      float noise_mid  = ((hiss_rng_state_ & 0x7fffffff) / 2147483647.0f) - 0.5f;
      noise_mid = tape_lp_l_.Process<stmlib::FILTER_MODE_HIGH_PASS>(noise_mid);
      noise_mid = tape_lp_r_.Process<stmlib::FILTER_MODE_LOW_PASS>(noise_mid);

      // --- VHS Snow Filtering ---
      float low_noise_L  = filter_[4].Process<stmlib::FILTER_MODE_LOW_PASS>(noise_mid);
      float high_noise_L = filter_[5].Process<stmlib::FILTER_MODE_HIGH_PASS>(noise_mid);
      float low_noise_R  = filter_[6].Process<stmlib::FILTER_MODE_LOW_PASS>(noise_mid);
      float high_noise_R = filter_[7].Process<stmlib::FILTER_MODE_HIGH_PASS>(noise_mid);

      // VHS-style mixed noise (quality drives snow character)
      float wear_mid = degradation * 0.7f;
      float wear_heavy = degradation * 0.4f;
      float stereo_mod = breathing * 0.15f;  // dynamic stereo: spreads when input is quiet

      float vhs_snow_L =
      low_noise_L  * (0.2f + wear_mid * 0.3f + stereo_mod)
      + noise_mid    * (0.6f + wear_heavy * 0.3f)
      + high_noise_L * (0.2f * wear_heavy - stereo_mod);

      float vhs_snow_R =
      low_noise_R  * (0.2f + wear_mid * 0.3f - stereo_mod)
      + noise_mid    * (0.6f + wear_heavy * 0.3f)
      + high_noise_R * (0.2f * wear_heavy + stereo_mod);

      // --- Noise Amount (quality drives noise floor, knob adds subtle extra) ---
      float noise_level = degradation * 0.15f + effect_amount * 0.03f;
      float noise_amount = noise_level * (0.8f + breathing * 1.2f);

      // --- Tracking Instability (far end of knob, >85%) ---
      float tracking_instability = 0.0f;
      if (effect_amount > 0.85f) {
          float tracking_amount = (effect_amount - 0.85f) / 0.15f;
          tracking_amount *= tracking_amount;  // quadratic ramp — gentle onset

          tracking_lfo_phase += (8.0f + effect_amount * 12.0f) * kSamplePeriod;
          if (tracking_lfo_phase >= 1.0f) tracking_lfo_phase -= 1.0f;
          float tracking_lfo = Interpolate(lut_sin, tracking_lfo_phase, 1024.0f);

          // Periodic tracking glitch: signal disruption when LFO > threshold
          tracking_instability = tracking_amount * (tracking_lfo > 0.3f ? 1.0f : 0.0f);
      }

      // --- Dropout Logic ---
      float dropout_target = 1.0f;

      if (dropout_duration_timer > 0.0f) {
        // Currently dropping out
        dropout_duration_timer -= kSamplePeriod;
        dropout_target = dropout_gain_min;
      } else if (tracking_instability > 0.5f) {
        // Tracking loss — partial signal drop with noise
        dropout_target = 0.3f + (1.0f - tracking_instability) * 0.4f;
      } else {
        // Check if we should start a new dropout
        dropout_check_timer -= kSamplePeriod;
        if (dropout_check_timer <= 0.0f) {
          if (Random::GetFloat() < dropout_base_chance) {
              dropout_duration_timer = 0.001f + Random::GetFloat() * dropout_max_length;
          }
          dropout_check_timer = 0.01f + (1.0f - effect_amount) * 0.05f;
        }
      }

      // Asymmetric envelope: fast attack (~2ms), slower release (~5ms)
      float dropout_coeff = (dropout_target < dropout_envelope) ? 0.005f : 0.002f;
      ONE_POLE(dropout_envelope, dropout_target, dropout_coeff);

      // Noise rises as signal falls (AGC simulation)
      float signal_loss = 1.0f - dropout_envelope;
      float dropout_noise = noise_mid * signal_loss * 0.05f;

      // Apply dropout with noise fill
      float signal_A = mix_A * dropout_envelope + dropout_noise;
      float signal_B = mix_B * dropout_envelope + dropout_noise;

      // --- Tape Saturation (quality drives it, knob adds subtle extra) ---
      float drive = 1.0f + degradation * 3.0f + effect_amount * 0.5f;
      signal_A = stmlib::SoftLimit(signal_A * drive);
      signal_B = stmlib::SoftLimit(signal_B * drive);

      // --- Tilt Filter ---
      float tilt_lpf_A = filter_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(signal_A);
      float tilt_hpf_A = filter_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(signal_A);
      float tilt_lpf_B = filter_[2].Process<stmlib::FILTER_MODE_LOW_PASS>(signal_B);
      float tilt_hpf_B = filter_[3].Process<stmlib::FILTER_MODE_HIGH_PASS>(signal_B);

      // Quality drives tilt, knob adds subtle extra
      // Dropout darkens progressively; highs recover slower than lows (head re-seating)
      float tilt_coeff = (signal_loss > tilt_envelope) ? 0.008f : 0.001f;
      ONE_POLE(tilt_envelope, signal_loss, tilt_coeff);
      float dropout_tilt_mod = tilt_envelope * 0.5f;
      float tilt_mix = 0.5f + degradation * 0.4f + effect_amount * 0.1f + dropout_tilt_mod;
      if (tilt_mix > 1.0f) tilt_mix = 1.0f;

      float filtered_A = tilt_hpf_A * (1.0f - tilt_mix) + tilt_lpf_A * tilt_mix;
      float filtered_B = tilt_hpf_B * (1.0f - tilt_mix) + tilt_lpf_B * tilt_mix;

      // --- 60Hz Hum ---
      float random_val = static_cast<float>((hiss_rng_state_ >> 9) & 0x7FFFFF) / 8388607.0f;
      static float hum_instability = 0.0f;
      float target = age_chewed * (random_val - 0.5f) * 0.02f;
      ONE_POLE(hum_instability, target, 0.0005f);

      float hum_freq = 60.0f + hum_instability;
      hum_lfo_phase_ += hum_freq * kSamplePeriod;
      if (hum_lfo_phase_ >= 1.0f) hum_lfo_phase_ -= 1.0f;
      float hum_l = Interpolate(lut_sin, hum_lfo_phase_, 1024.0f);
      hum_l = hum_l * (1.0f + 0.25f * hum_l);

      // 90° phase-shifted hum for R — rotates in stereo field
      float hum_phase_r = hum_lfo_phase_ + 0.25f;
      if (hum_phase_r >= 1.0f) hum_phase_r -= 1.0f;
      float hum_r = Interpolate(lut_sin, hum_phase_r, 1024.0f);
      hum_r = hum_r * (1.0f + 0.25f * hum_r);

      // Quality drives hum level, knob adds subtle extra
      float hum_amount = degradation * 0.05f + effect_amount * 0.01f;

      // --- Head-Switch Click (drum head switches every 1/60s) ---
      // Detect hum phase wrap = head switch moment
      if (hum_lfo_phase_ < prev_hum_phase) {
          click_envelope = 1.0f;
      }
      prev_hum_phase = hum_lfo_phase_;
      click_envelope *= 0.92f;  // ~0.5ms decay
      float head_click = noise_mid * click_envelope * degradation * 0.15f;

      // --- Add Noise, Hum & Click ---
      processed_A = filtered_A + (vhs_snow_L * noise_amount * 0.04f) + (hum_l * hum_amount) + head_click;
      processed_B = filtered_B + (vhs_snow_R * noise_amount * 0.04f) + (hum_r * hum_amount) + head_click;
    }

    delay_buffer_[shared_write_pos_].l = Clip16(mix_A * 32768.0f);
    delay_buffer_[shared_write_pos_].r = Clip16(mix_B * 32768.0f);
    if (++shared_write_pos_ >= kSharedDelaySize) shared_write_pos_ = 0;

    MixerWriteOutput(output++, f, processed_A, processed_B);
  }
  MixerFinalize();
}

void Modulator::ProcessLossyMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  MixerPrepareInputs(input, size);

  float algo_start = SkewRawAlgorithm(previous_parameters_.raw_algorithm);
  float algo_end = SkewRawAlgorithm(parameters_.raw_algorithm);
  ParameterInterpolator algo_interp(&algo_start, algo_end, size);
  ParameterInterpolator xfade_interp(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter, size);

    // carrier_shape: 0=off(best), 1=green, 2=yellow, 3=red(most lofi)
    int quality = 3 - parameters_.carrier_shape;  // 3=best, 0=lofi
    const float degradation = 1.0f - (quality / 3.0f);  // hoist: 0=q3, 1.0=q0
    const int write_decimation = (quality == 0) ? 4 : (quality == 1) ? 3 : (quality == 2) ? 2 : 1;
    const float lp_coeffs[] = { 0.12f, 0.25f, 0.5f, 0.8f };
    const float fb_lp_coeff = lp_coeffs[quality];
    const float max_birdie_resonance = degradation * 35.0f;
    const float birdie_mix_scale = degradation * 0.65f;
    const int decimation_factor = (quality == 0) ? 8 : (quality == 1) ? 5 : (quality == 2) ? 3 : 1;
    const float bit_scales[] = { 256.0f, 1024.0f, 4096.0f, 1.0f };
    const float bit_scale = bit_scales[quality];
    const float bit_scale_inv = 1.0f / bit_scale;
    const bool do_bitcrush = (quality < 3);

  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];

  while (size--) {
    bool write_to_buffer = true;

    MixerFrame f = MixerComputeFrame(
        *in_1++, *in_2++, xfade_interp.Next(), algo_interp.Next());
    float mix_A = f.mix_A;
    float mix_B = f.mix_B;
    float effect_amount = f.effect_amount;

    // --- Write clock: all quality levels get sample rate reduction ---
    static int write_counter = 0;
    bool write_clock = (++write_counter >= write_decimation);
    if (write_clock) write_counter = 0;

    float processed_A = 0.0f;
    float processed_B = 0.0f;

      if (f.algo < 0.0f) {
        // --- CCW: Corrupt (MP3/codec breaking) ---
        static float corrupt_read_pos = 0.0f;
        static float corrupt_read_speed = 1.0f;
        static float frozen_l = 0.0f, frozen_r = 0.0f;
        static float freeze_timer = 0.0f;
        static bool frame_frozen = false;
        static float resync_timer = 0.0f;
        static uint32_t corrupt_rng = 0x12345678;

        // LCG noise for corruption
        corrupt_rng = corrupt_rng * 1664525u + 1013904223u;

        const float buffer_size_f = (float)kSharedDelaySize;

        // --- Sample rate mismatch: read at wrong speed ---
        // q3: barely off (0.98-1.02), q0: very wrong (0.7-1.3)
        const float speed_ranges[] = { 0.30f, 0.20f, 0.10f, 0.02f };
        float speed_range = speed_ranges[quality];

        // Periodically resync and pick a new wrong speed
        resync_timer -= kSamplePeriod;
        if (resync_timer <= 0.0f) {
            // Resync: jump read pos back to near write pos
            float resync_strength = effect_amount * 0.8f;
            float target = (float)shared_write_pos_ - 96.0f;  // 1ms behind
            if (target < 0.0f) target += buffer_size_f;
            corrupt_read_pos += (target - corrupt_read_pos) * resync_strength;
            if (corrupt_read_pos < 0.0f) corrupt_read_pos += buffer_size_f;
            if (corrupt_read_pos >= buffer_size_f) corrupt_read_pos -= buffer_size_f;

            // Pick new wrong speed
            float rnd = (float)(corrupt_rng & 0x7fffffff) / 2147483647.0f;
            corrupt_read_speed = 1.0f + (rnd - 0.5f) * 2.0f * speed_range * effect_amount;

            // Resync interval: shorter at higher effect
            resync_timer = 0.05f + (1.0f - effect_amount) * 0.2f;
        }

        // Advance corrupted read position
        corrupt_read_pos += corrupt_read_speed;
        if (corrupt_read_pos >= buffer_size_f) corrupt_read_pos -= buffer_size_f;
        if (corrupt_read_pos < 0.0f) corrupt_read_pos += buffer_size_f;

        // --- Random position jumps (frame corruption) ---
        float jump_chance = effect_amount * effect_amount * 0.005f * (1.0f + degradation * 3.0f);
        float rnd_jump = (float)((corrupt_rng >> 8) & 0x7fffff) / 8388607.0f;
        if (rnd_jump < jump_chance) {
            corrupt_rng = corrupt_rng * 1664525u + 1013904223u;
            float jump_offset = (float)(corrupt_rng % kSharedDelaySize);
            corrupt_read_pos = jump_offset;
        }

        // --- Read from corrupted position ---
        float rp = corrupt_read_pos;
        float read_l, read_r;
        {
            MAKE_INTEGRAL_FRACTIONAL(rp);
            int32_t i0 = rp_integral;
            int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
            read_l = (delay_buffer_[i0].l + (delay_buffer_[i1].l - delay_buffer_[i0].l) * rp_fractional) * kToFloat;
            read_r = (delay_buffer_[i0].r + (delay_buffer_[i1].r - delay_buffer_[i0].r) * rp_fractional) * kToFloat;
        }

        // --- Bit manipulation (XOR corruption) ---
        if (effect_amount > 0.2f) {
            // Corrupt the integer representation
            const uint32_t masks[] = { 0xFF00, 0xFC00, 0xF000, 0xC000 };
            uint32_t mask = masks[quality];
            float corruption_amount = (effect_amount - 0.2f) / 0.8f;

            float rnd_bit = (float)((corrupt_rng >> 16) & 0xFF) / 255.0f;
            if (rnd_bit < corruption_amount * 0.3f) {
                int16_t sl = (int16_t)(read_l * 32768.0f);
                int16_t sr = (int16_t)(read_r * 32768.0f);
                sl ^= (int16_t)(corrupt_rng & mask);
                sr ^= (int16_t)((corrupt_rng >> 4) & mask);
                read_l = (float)sl * kToFloat;
                read_r = (float)sr * kToFloat;
            }
        }

        // --- Frame freeze (frozen MP3 frame) ---
        freeze_timer -= kSamplePeriod;
        if (!frame_frozen) {
            float freeze_chance = effect_amount * effect_amount * 0.01f * (1.0f + degradation * 2.0f);
            float rnd_frz = (float)((corrupt_rng >> 12) & 0xFFF) / 4095.0f;
            if (rnd_frz < freeze_chance) {
                frame_frozen = true;
                frozen_l = read_l;
                frozen_r = read_r;
                // Freeze duration: 5-50ms
                freeze_timer = 0.005f + (float)((corrupt_rng >> 20) & 0xFF) / 255.0f * 0.045f;
            }
        }
        if (frame_frozen) {
            read_l = frozen_l;
            read_r = frozen_r;
            if (freeze_timer <= 0.0f) {
                frame_frozen = false;
            }
        }

        // --- Codec reconstruction lowpass ---
        {
            static float recon_lp_l = 0.0f, recon_lp_r = 0.0f;
            float recon_coeff = fb_lp_coeff;  // quality-dependent
            ONE_POLE(recon_lp_l, read_l, recon_coeff);
            ONE_POLE(recon_lp_r, read_r, recon_coeff);
            read_l = recon_lp_l;
            read_r = recon_lp_r;
        }

        processed_A = read_l;
        processed_B = read_r;
      } else {
        // --- CW: Glitches ---
        float effect_amount_sqrtf = sqrtf(effect_amount);
        static int decimation_counter_ = 0;
        static float latched_l_ = 0.0f;
        static float latched_r_ = 0.0f;
        static bool decimator_needs_reset_ = true;

        // Decimator: quality 0=lofi(÷8=12kHz), 1=(÷5=~19kHz), 2=(÷3=32kHz), 3=(÷2=48kHz)
        if (++decimation_counter_ >= decimation_factor) decimation_counter_ = 0;
        if (decimation_counter_ == 0 || decimator_needs_reset_) {
            // "Latch" a new sample
            latched_l_ = mix_A;
            latched_r_ = mix_B;
            decimator_needs_reset_ = false;
        }

        // --- Static variables ---
        static float glitch_timer = 0.0f;
        static bool currently_glitching = false;
        static float glitch_hold_duration = 0.0f;
        static bool pitch_glitching = false;
        static float glitch_read_pos_f = 0.0f;
        static float glitch_read_speed = 0.5f;
        static bool micro_looping = false;
        static float loop_read_pos_f = 0.0f;
        static float loop_start_pos_f = 0.0f;
        static float loop_length_f = 0.0f;

        static int birdie_update_counter_ = 0;
        static float birdie_lfo_phase_ = 0.0f;
        static float birdie_lfo_target_val_ = 0.5f;
        static float birdie_lfo_current_val_ = 0.5f;
        static float loop_read_speed = 1.0f;
        static bool force_loop_needs_capture = true;
        const float force_loop_threshold = 0.99f;
        static float force_loop_crossfade_center_ = 0.5f;

        const float silence_threshold = 0.0001f;
        bool is_silent = (fabs(mix_A) < silence_threshold) && (fabs(mix_B) < silence_threshold);

        glitch_timer -= kSamplePeriod;

        // --- Glitch Trigger Logic ---
        if (effect_amount >= force_loop_threshold) {
            // --- FORCED LOOP ZONE ---
            
            write_to_buffer = false; 
            currently_glitching = true;
            pitch_glitching = false;
            micro_looping = true;
            
            if (force_loop_needs_capture && !is_silent) {
                loop_length_f = 11520.0f; // 120ms
                loop_start_pos_f = (float)(static_cast<int32_t>(shared_write_pos_ - loop_length_f + kSharedDelaySize) % kSharedDelaySize);
                loop_read_pos_f = loop_start_pos_f;
                force_loop_crossfade_center_ = f.crossfade;
                force_loop_needs_capture = false;
            }

            float delta = f.crossfade - force_loop_crossfade_center_;
            const float speed_sensitivity = 2.0f;
            loop_read_speed = 1.0f + delta * speed_sensitivity;
        } else {
            // --- NORMAL PROBABILISTIC ZONE (0% to 99%) ---
            
            force_loop_needs_capture = true; 
            
            if (currently_glitching) {
                glitch_hold_duration -= kSamplePeriod;
                if (glitch_hold_duration <= 0.0f || is_silent) {
                    currently_glitching = false;
                    pitch_glitching = false;
                    micro_looping = false;
                    glitch_timer = 0.005f + Random::GetFloat() * 0.01f;
                }
            } else {
                if (glitch_timer <= 0.0f) {
                  
                  float glitch_chance = effect_amount_sqrtf * 0.25f;
                  
                  if (Random::GetFloat() < glitch_chance && !is_silent) {
                    
                    currently_glitching = true;
                    
                    float max_duration = 3.0f * effect_amount;
                    glitch_hold_duration = 0.05f + Random::GetFloat() * max_duration; 
                    
                    const float pitch_threshold = 0.5f;
                    pitch_glitching = false;
                    micro_looping = false;
                    
                    if (effect_amount < pitch_threshold) {
                        micro_looping = true;
                    } else {
                        float pitch_chance = (effect_amount - pitch_threshold) * 2.0f;
                        if (Random::GetFloat() < pitch_chance) {
                            pitch_glitching = true;
                        } else {
                            micro_looping = true;
                        }
                    }

                    if (pitch_glitching) {
                        float random_mod = Random::GetFloat() * 0.1f - 0.05f;
                        int speed_choice = static_cast<int>(Random::GetFloat() * 3.0f);
                        
                        if (speed_choice == 0)      glitch_read_speed = 0.25f + random_mod;
                        else if (speed_choice == 1) glitch_read_speed = 0.125f + random_mod;
                        else                        glitch_read_speed = -0.5f + random_mod;
                        
                        int32_t random_offset = 100 + static_cast<int>(Random::GetFloat() * (kSharedDelaySize - 200));
                        glitch_read_pos_f = (float)((shared_write_pos_ - random_offset + kSharedDelaySize) % kSharedDelaySize);

                    } else { // This is a micro_loop
                        loop_read_speed = 1.0f;
                        
                        // Was 24000.0f (0.25 sec). Let's try 96000.0f (1 sec).
                        float max_loop_length = 96000.0f * effect_amount;
                        loop_length_f = 4800.0f + Random::GetFloat() * max_loop_length; 
                        int32_t random_offset = 100 + static_cast<int>(Random::GetFloat() * 4700.0f);
                        loop_start_pos_f = (float)(static_cast<int32_t>(shared_write_pos_ - random_offset + kSharedDelaySize) % kSharedDelaySize);
                        loop_read_pos_f = loop_start_pos_f;
                    }
                  } else {
                    glitch_timer = 0.04f + Random::GetFloat() * (0.1f * (1.0f - effect_amount));
                  }
                }
            }
        }
        
        // --- Glitch "Player" Logic ---
        float glitch_l = 0.0f, glitch_r = 0.0f;

        if (currently_glitching) {
            if (pitch_glitching) {
                // --- PITCH GLITCH ---
                glitch_read_pos_f += glitch_read_speed;
                while (glitch_read_pos_f < 0.0f) {
                  glitch_read_pos_f += kSharedDelaySize;
                }
                while (glitch_read_pos_f >= kSharedDelaySize) {
                  glitch_read_pos_f -= kSharedDelaySize;
                }
                
                MAKE_INTEGRAL_FRACTIONAL(glitch_read_pos_f);
                {
                    int32_t index_integral = glitch_read_pos_f_integral;
                    float index_fractional = glitch_read_pos_f_fractional;
          

                    if (quality == 0) {
                        // Nearest-neighbor: chunky, aliased
                        int32_t idx = (index_fractional > 0.5f) ? ((index_integral + 1) % kSharedDelaySize) : index_integral;
                        glitch_l = static_cast<float>(delay_buffer_[idx].l) * kToFloat;
                        glitch_r = static_cast<float>(delay_buffer_[idx].r) * kToFloat;
                    } else if (quality == 1) {
                        // Linear interpolation: rougher than Hermite
                        float x0_l = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
                        float x1_l = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].l) * kToFloat;
                        float x0_r = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
                        float x1_r = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].r) * kToFloat;
                        glitch_l = x0_l + (x1_l - x0_l) * index_fractional;
                        glitch_r = x0_r + (x1_r - x0_r) * index_fractional;
                    } else {
                        // Hermite (4-point cubic): smooth
                        float xm1_l = static_cast<float>(delay_buffer_[(index_integral - 1 + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
                        float x0_l  = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
                        float x1_l  = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].l) * kToFloat;
                        float x2_l  = static_cast<float>(delay_buffer_[(index_integral + 2) % kSharedDelaySize].l) * kToFloat;
                        float xm1_r = static_cast<float>(delay_buffer_[(index_integral - 1 + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
                        float x0_r  = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
                        float x1_r  = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].r) * kToFloat;
                        float x2_r  = static_cast<float>(delay_buffer_[(index_integral + 2) % kSharedDelaySize].r) * kToFloat;
                        FloatFrame c = { (x1_l - xm1_l) * 0.5f, (x1_r - xm1_r) * 0.5f };
                        FloatFrame v = { (float)(x0_l - x1_l), (float)(x0_r - x1_r)};
                        FloatFrame w = { c.l + v.l, c.r + v.r };
                        FloatFrame a = { w.l + v.l + (x2_l - x0_l) * 0.5f, w.r + v.r + (x2_r - x0_r) * 0.5f };
                        FloatFrame b_neg = { w.l + a.l, w.r + a.r };
                        float t = index_fractional;
                        glitch_l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0_l);
                        glitch_r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0_r);
                    }
                }

            } else if (micro_looping) {
                // --- MICRO-LOOP ---
                loop_read_pos_f += loop_read_speed; 
                
                float loop_end_pos = loop_start_pos_f + loop_length_f;
                
                if (loop_read_speed > 0.0f && loop_read_pos_f >= loop_end_pos) {
                    loop_read_pos_f -= loop_length_f;
                } else if (loop_read_speed < 0.0f && loop_read_pos_f < loop_start_pos_f) {
                    loop_read_pos_f += loop_length_f;
                }
                
                float read_pos_for_hermite = loop_read_pos_f;
                while (read_pos_for_hermite >= (float)kSharedDelaySize) read_pos_for_hermite -= (float)kSharedDelaySize;
                while (read_pos_for_hermite < 0.0f) read_pos_for_hermite += (float)kSharedDelaySize;

                MAKE_INTEGRAL_FRACTIONAL(read_pos_for_hermite);
                {
                    int32_t index_integral = read_pos_for_hermite_integral;
                    float index_fractional = read_pos_for_hermite_fractional;
          

                    if (quality == 0) {
                        // Nearest-neighbor
                        int32_t idx = (index_fractional > 0.5f) ? ((index_integral + 1) % kSharedDelaySize) : index_integral;
                        glitch_l = static_cast<float>(delay_buffer_[idx].l) * kToFloat;
                        glitch_r = static_cast<float>(delay_buffer_[idx].r) * kToFloat;
                    } else if (quality == 1) {
                        // Linear interpolation
                        float x0_l = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
                        float x1_l = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].l) * kToFloat;
                        float x0_r = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
                        float x1_r = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].r) * kToFloat;
                        glitch_l = x0_l + (x1_l - x0_l) * index_fractional;
                        glitch_r = x0_r + (x1_r - x0_r) * index_fractional;
                    } else {
                        // Hermite (4-point cubic)
                        float xm1_l = static_cast<float>(delay_buffer_[(index_integral - 1 + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
                        float x0_l  = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
                        float x1_l  = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].l) * kToFloat;
                        float x2_l  = static_cast<float>(delay_buffer_[(index_integral + 2) % kSharedDelaySize].l) * kToFloat;
                        float xm1_r = static_cast<float>(delay_buffer_[(index_integral - 1 + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
                        float x0_r  = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
                        float x1_r  = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].r) * kToFloat;
                        float x2_r  = static_cast<float>(delay_buffer_[(index_integral + 2) % kSharedDelaySize].r) * kToFloat;
                        FloatFrame c = { (x1_l - xm1_l) * 0.5f, (x1_r - xm1_r) * 0.5f };
                        FloatFrame v = { (float)(x0_l - x1_l), (float)(x0_r - x1_r)};
                        FloatFrame w = { c.l + v.l, c.r + v.r };
                        FloatFrame a = { w.l + v.l + (x2_l - x0_l) * 0.5f, w.r + v.r + (x2_r - x0_r) * 0.5f };
                        FloatFrame b_neg = { w.l + a.l, w.r + a.r };
                        float t = index_fractional;
                        glitch_l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0_l);
                        glitch_r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0_r);
                    }
                }
            }
        }
        
        // --- "Birdie" ---
        birdie_lfo_phase_ += (0.5f + effect_amount * 3.0f) * kSamplePeriod;
        if (birdie_lfo_phase_ >= 1.0f) {
            birdie_lfo_phase_ -= 1.0f;
            birdie_lfo_target_val_ = Random::GetFloat();
        }
        ONE_POLE(birdie_lfo_current_val_, birdie_lfo_target_val_, 0.001f);

        const int kBirdieUpdateRate = 32; 
        if (birdie_update_counter_ == 0) {
          // Mirrored birdie cutoff: L and R sweep in opposite directions
          float bpf_cutoff_l = 0.1f + (birdie_lfo_current_val_ * 0.2f);
          float bpf_cutoff_r = 0.1f + ((1.0f - birdie_lfo_current_val_) * 0.2f);
          // Birdie resonance: off at q3, screeching at q0
          float bpf_resonance = 1.0f + effect_amount_sqrtf * max_birdie_resonance;

          lossy_bpf_l_.set_f_q<stmlib::FREQUENCY_FAST>(bpf_cutoff_l, bpf_resonance);
          lossy_bpf_r_.set_f_q<stmlib::FREQUENCY_FAST>(bpf_cutoff_r, bpf_resonance);
        }

        // Increment and wrap the counter every sample
        if (++birdie_update_counter_ >= kBirdieUpdateRate) birdie_update_counter_ = 0;
        
        float base_signal_l = (currently_glitching) ? glitch_l : latched_l_;
        float base_signal_r = (currently_glitching) ? glitch_r : latched_r_;

        float birdie_l = lossy_bpf_l_.Process<stmlib::FILTER_MODE_BAND_PASS>(base_signal_l);
        float birdie_r = lossy_bpf_r_.Process<stmlib::FILTER_MODE_BAND_PASS>(base_signal_r);
        
        // --- Final Mix: no birdie at q3, intense at q0 ---
        float birdie_mix = effect_amount_sqrtf * birdie_mix_scale;

        processed_A = base_signal_l * (1.0f - birdie_mix) + birdie_l * birdie_mix;
        processed_B = base_signal_r * (1.0f - birdie_mix) + birdie_r * birdie_mix;

        // --- Bit crushing: quality 3=none, 2=12bit, 1=10bit, 0=8bit ---
        if (do_bitcrush) {
            processed_A = floorf(processed_A * bit_scale + 0.5f) * bit_scale_inv;
            processed_B = floorf(processed_B * bit_scale + 0.5f) * bit_scale_inv;
        }
      }

    // --- FINAL MIX ---
    if (write_to_buffer) {
        static int16_t last_written_l = 0;
        static int16_t last_written_r = 0;

        if (write_clock) {
            last_written_l = Clip16(mix_A * 32768.0f);
            last_written_r = Clip16(mix_B * 32768.0f);
        }

        delay_buffer_[shared_write_pos_].l = last_written_l;
        delay_buffer_[shared_write_pos_].r = last_written_r;

        shared_write_pos_++;
        if (shared_write_pos_ >= kSharedDelaySize) shared_write_pos_ = 0;
    }

    MixerWriteOutput(output++, f, processed_A, processed_B);
  }
  MixerFinalize();
}

void Modulator::ProcessRadioMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  MixerPrepareInputs(input, size);
  int32_t radio_quality = parameters_.carrier_shape;
  float degradation = radio_quality / 3.0f;

  float algo_start = SkewRawAlgorithm(previous_parameters_.raw_algorithm);
  float algo_end = SkewRawAlgorithm(parameters_.raw_algorithm);
  ParameterInterpolator algo_interp(&algo_start, algo_end, size);
  ParameterInterpolator xfade_interp(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter, size);

  static float comb_feedback_l = 0.0f;
  static float comb_feedback_r = 0.0f;
  static float jitter_lp = 0.0f;
  static int jitter_counter = 0;

  float bw_cutoff = 0.35f - degradation * 0.2f;
  filter_[6].set_f<stmlib::FREQUENCY_FAST>(bw_cutoff);
  filter_[7].set_f<stmlib::FREQUENCY_FAST>(bw_cutoff);

  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];

  while (size--) {
    MixerFrame f = MixerComputeFrame(
        *in_1++, *in_2++, xfade_interp.Next(), algo_interp.Next());

    float processed_A = 0.0f;
    float processed_B = 0.0f;

    if (f.algo < 0.0f) {
      // --- CCW: AM/FM Radio Demodulation Artifacts ---
      float bw_limited_A = filter_[6].Process<stmlib::FILTER_MODE_LOW_PASS>(f.mix_A);
      float bw_limited_B = filter_[7].Process<stmlib::FILTER_MODE_LOW_PASS>(f.mix_B);
      float bw_mix = f.effect_amount * 0.8f;
      float band_A = f.mix_A + (bw_limited_A - f.mix_A) * bw_mix;
      float band_B = f.mix_B + (bw_limited_B - f.mix_B) * bw_mix;

      float carrier_freq = 0.15f + f.effect_amount * 0.2f;
      radio_carrier_phase_ += carrier_freq;
      if (radio_carrier_phase_ > 1.0f) radio_carrier_phase_ -= 1.0f;
      float cp = radio_carrier_phase_ * 2.0f - 1.0f;
      float carrier_tone = 4.0f * cp * (1.0f - fabs(cp));
      float carrier_bleed = carrier_tone * f.effect_amount * degradation * 0.15f;

      float rect_A = fabs(band_A);
      float rect_B = fabs(band_B);
      float rect_mix = f.effect_amount * 0.3f * degradation;

      float static_raw = (Random::GetFloat() - 0.5f) * 2.0f;
      ONE_POLE(radio_static_lp_, static_raw, 0.3f);
      float static_amount = f.effect_amount * (0.05f + degradation * 0.3f);

      radio_fade_lfo_phase_ += 0.00003f + degradation * 0.00005f;
      if (radio_fade_lfo_phase_ > 1.0f) radio_fade_lfo_phase_ -= 1.0f;
      float fade_lfo = radio_fade_lfo_phase_ * 2.0f - 1.0f;
      fade_lfo = 4.0f * fade_lfo * (1.0f - fabs(fade_lfo));
      float fade_depth = f.effect_amount * degradation * 0.4f;
      float fade_gain = 1.0f - fade_depth * (0.5f + 0.5f * fade_lfo);

      processed_A = (band_A * (1.0f - rect_mix) + rect_A * rect_mix) * fade_gain
                   + carrier_bleed + radio_static_lp_ * static_amount;
      processed_B = (band_B * (1.0f - rect_mix) + rect_B * rect_mix) * fade_gain
                   + carrier_bleed * 0.7f + radio_static_lp_ * static_amount * 0.8f;

    } else {
      // --- CW: Comb Filter + Jitter Resonance (Multipath Interference) ---
      float inv = (1.0f - f.effect_amount);
      float delay_samples = 48.0f + (1920.0f - 48.0f) * inv * inv;

      if (++jitter_counter >= 4) {
          jitter_counter = 0;
          float jitter_raw = (Random::GetFloat() - 0.5f) * 2.0f;
          ONE_POLE(jitter_lp, jitter_raw, 0.18f);
      }

      float jitter_depth;
      if (radio_quality == 0) jitter_depth = 0.0f;
      else if (radio_quality == 1) jitter_depth = 0.5f;
      else if (radio_quality == 2) jitter_depth = 2.0f;
      else jitter_depth = 6.0f;

      float jitter_amount = jitter_lp * jitter_depth * f.effect_amount;
      float jittered_delay_l = delay_samples + jitter_amount;
      float jittered_delay_r = delay_samples - jitter_amount * 0.6f;
      if (jittered_delay_l < 1.0f) jittered_delay_l = 1.0f;
      if (jittered_delay_r < 1.0f) jittered_delay_r = 1.0f;

      float feedback_amount = 0.6f + f.effect_amount * 0.32f;

      float write_l = f.mix_A + comb_feedback_l * feedback_amount;
      float write_r = f.mix_B + comb_feedback_r * feedback_amount;
      delay_buffer_[shared_write_pos_].l = Clip16(write_l * 32768.0f);
      delay_buffer_[shared_write_pos_].r = Clip16(write_r * 32768.0f);

      float read_pos_l = (float)shared_write_pos_ - jittered_delay_l;
      if (read_pos_l < 0.0f) read_pos_l += kSharedDelaySize;
      MAKE_INTEGRAL_FRACTIONAL(read_pos_l);
      int32_t i0_l = read_pos_l_integral;
      int32_t i1_l = i0_l + 1; if (i1_l >= kSharedDelaySize) i1_l = 0;
      float delayed_l = delay_buffer_[i0_l].l * kToFloat
          + (delay_buffer_[i1_l].l * kToFloat - delay_buffer_[i0_l].l * kToFloat)
          * read_pos_l_fractional;

      float read_pos_r = (float)shared_write_pos_ - jittered_delay_r;
      if (read_pos_r < 0.0f) read_pos_r += kSharedDelaySize;
      MAKE_INTEGRAL_FRACTIONAL(read_pos_r);
      int32_t i0_r = read_pos_r_integral;
      int32_t i1_r = i0_r + 1; if (i1_r >= kSharedDelaySize) i1_r = 0;
      float delayed_r = delay_buffer_[i0_r].r * kToFloat
          + (delay_buffer_[i1_r].r * kToFloat - delay_buffer_[i0_r].r * kToFloat)
          * read_pos_r_fractional;

      comb_feedback_l = stmlib::SoftLimit(delayed_l);
      comb_feedback_r = stmlib::SoftLimit(delayed_r);

      shared_write_pos_++;
      if (shared_write_pos_ >= kSharedDelaySize) shared_write_pos_ = 0;

      float comb_mix = 0.5f + f.effect_amount * 0.3f;
      processed_A = f.mix_A * (1.0f - comb_mix) + delayed_l * comb_mix;
      processed_B = f.mix_B * (1.0f - comb_mix) + delayed_r * comb_mix;
    }

    MixerWriteOutput(output++, f, processed_A, processed_B);
  }
  MixerFinalize();
}

void Modulator::ProcessDreamyMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  MixerPrepareInputs(input, size);

  float algo_start = SkewRawAlgorithm(previous_parameters_.raw_algorithm);
  float algo_end = SkewRawAlgorithm(parameters_.raw_algorithm);
  ParameterInterpolator algo_interp(&algo_start, algo_end, size);
  ParameterInterpolator xfade_interp(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter, size);

  int quality = 3 - parameters_.carrier_shape;
  const float degradation = 1.0f - (quality / 3.0f);
  const float mod_scale = 1.0f + degradation * 2.0f;
  const float smear_drive = 1.0f + degradation * 3.0f;
  const float lp_coeffs[] = { 0.12f, 0.25f, 0.5f, 0.8f };
  const float fb_lp_coeff = lp_coeffs[quality];

  const float kToFloat = 1.0f / 32768.0f;

  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];

  while (size--) {
    MixerFrame f = MixerComputeFrame(
        *in_1++, *in_2++, xfade_interp.Next(), algo_interp.Next());
    float mix_A = f.mix_A;
    float mix_B = f.mix_B;
    float effect_amount = f.effect_amount;

    float processed_A = 0.0f;
    float processed_B = 0.0f;

    if (f.algo < 0.0f) {
      // --- CCW: Granular Smear / Chorus ---
      static float feedback_l = 0.0f, feedback_r = 0.0f;
      static float smear_lfo_phase_ = 0.0f;
      static float smear_lfo_target_val_ = 0.5f;
      static float smear_lfo_current_val_ = 0.5f;
      static float chorus_lfo_phase_ = 0.0f;
      static float pitch_offset_a_ = 0.0f;
      static float pitch_offset_b_ = 0.0f;

      float base_delay_samples = 384.0f + effect_amount * 2496.0f;

      // Update Smear LFO - random walk modulates delay time
      smear_lfo_phase_ += 0.6f * kSamplePeriod;
      if (smear_lfo_phase_ >= 1.0f) {
          smear_lfo_phase_ -= 1.0f;
          float new_target = Random::GetFloat();
          const float max_jump = 0.3f;
          if (new_target > smear_lfo_current_val_ + max_jump)
              new_target = smear_lfo_current_val_ + max_jump;
          else if (new_target < smear_lfo_current_val_ - max_jump)
              new_target = smear_lfo_current_val_ - max_jump;
          smear_lfo_target_val_ = new_target;
      }
      ONE_POLE(smear_lfo_current_val_, smear_lfo_target_val_, 0.001f);
      float smear_mod = (smear_lfo_current_val_ - 0.5f) * 2.0f;

      // Update Chorus LFO
      chorus_lfo_phase_ += 0.8f * kSamplePeriod;
      if (chorus_lfo_phase_ >= 1.0f) chorus_lfo_phase_ -= 1.0f;
      float chorus_mod = Interpolate(lut_sin, chorus_lfo_phase_, 1024.0f);

      float smear_mod_amount = effect_amount * 768.0f * mod_scale;
      float stereo_offset = (40.0f + chorus_mod * 60.0f) * effect_amount * mod_scale;

      float delay_l_samples = base_delay_samples - stereo_offset + (smear_mod * smear_mod_amount);
      float delay_r_samples = base_delay_samples + stereo_offset - (smear_mod * smear_mod_amount);

      const float min_delay = 32.0f;
      delay_l_samples = fmaxf(delay_l_samples, min_delay);
      delay_r_samples = fmaxf(delay_r_samples, min_delay);

      const float pitch_threshold = 0.8f;
      float pitch_shift_rate = 0.0f;
      const float buffer_size_f = (float)kSharedDelaySize;
      const float half_buffer = buffer_size_f * 0.5f;

      if (effect_amount > pitch_threshold) {
          float pitch_ramp = (effect_amount - pitch_threshold) / (1.0f - pitch_threshold);
          pitch_shift_rate = 0.498f * pitch_ramp;
      } else {
          pitch_offset_a_ = 0.0f;
          pitch_offset_b_ = half_buffer;
      }

      pitch_offset_a_ += pitch_shift_rate;
      pitch_offset_b_ += pitch_shift_rate;

      if (pitch_offset_a_ >= buffer_size_f) pitch_offset_a_ -= buffer_size_f;
      if (pitch_offset_b_ >= buffer_size_f) pitch_offset_b_ -= buffer_size_f;

      float delayed_l, delayed_r;

      if (pitch_shift_rate > 0.0f) {
          // Dual-grain pitch shifter (tick-free)
          float phase_a = pitch_offset_a_ / buffer_size_f;
          float window_a = 1.0f - fabsf(2.0f * phase_a - 1.0f);

          float phase_b = pitch_offset_b_ / buffer_size_f;
          float window_b = 1.0f - fabsf(2.0f * phase_b - 1.0f);

          // Pre-compute base read positions per channel
          float base_l = (float)shared_write_pos_ - delay_l_samples;
          if (base_l < 0.0f) base_l += buffer_size_f;
          float base_r = (float)shared_write_pos_ - delay_r_samples;
          if (base_r < 0.0f) base_r += buffer_size_f;

          // Grain A
          float rp_la = base_l + pitch_offset_a_;
          if (rp_la >= buffer_size_f) rp_la -= buffer_size_f;
          float rp_ra = base_r + pitch_offset_a_;
          if (rp_ra >= buffer_size_f) rp_ra -= buffer_size_f;

          float grain_a_l, grain_a_r;
          {
              MAKE_INTEGRAL_FRACTIONAL(rp_la);
              int32_t i0 = rp_la_integral;
              int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
              grain_a_l = (delay_buffer_[i0].l + (delay_buffer_[i1].l - delay_buffer_[i0].l) * rp_la_fractional) * kToFloat;
          }
          {
              MAKE_INTEGRAL_FRACTIONAL(rp_ra);
              int32_t i0 = rp_ra_integral;
              int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
              grain_a_r = (delay_buffer_[i0].r + (delay_buffer_[i1].r - delay_buffer_[i0].r) * rp_ra_fractional) * kToFloat;
          }

          // Grain B
          float rp_lb = base_l + pitch_offset_b_;
          if (rp_lb >= buffer_size_f) rp_lb -= buffer_size_f;
          float rp_rb = base_r + pitch_offset_b_;
          if (rp_rb >= buffer_size_f) rp_rb -= buffer_size_f;

          float grain_b_l, grain_b_r;
          {
              MAKE_INTEGRAL_FRACTIONAL(rp_lb);
              int32_t i0 = rp_lb_integral;
              int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
              grain_b_l = (delay_buffer_[i0].l + (delay_buffer_[i1].l - delay_buffer_[i0].l) * rp_lb_fractional) * kToFloat;
          }
          {
              MAKE_INTEGRAL_FRACTIONAL(rp_rb);
              int32_t i0 = rp_rb_integral;
              int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
              grain_b_r = (delay_buffer_[i0].r + (delay_buffer_[i1].r - delay_buffer_[i0].r) * rp_rb_fractional) * kToFloat;
          }

          delayed_l = grain_a_l * window_a + grain_b_l * window_b;
          delayed_r = grain_a_r * window_a + grain_b_r * window_b;
      } else {
          float rp_l = (float)shared_write_pos_ - delay_l_samples;
          if (rp_l >= buffer_size_f) rp_l -= buffer_size_f;
          else if (rp_l < 0.0f) rp_l += buffer_size_f;
          float rp_r = (float)shared_write_pos_ - delay_r_samples;
          if (rp_r >= buffer_size_f) rp_r -= buffer_size_f;
          else if (rp_r < 0.0f) rp_r += buffer_size_f;

          {
              MAKE_INTEGRAL_FRACTIONAL(rp_l);
              int32_t i0 = rp_l_integral;
              int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
              delayed_l = (delay_buffer_[i0].l + (delay_buffer_[i1].l - delay_buffer_[i0].l) * rp_l_fractional) * kToFloat;
          }
          {
              MAKE_INTEGRAL_FRACTIONAL(rp_r);
              int32_t i0 = rp_r_integral;
              int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
              delayed_r = (delay_buffer_[i0].r + (delay_buffer_[i1].r - delay_buffer_[i0].r) * rp_r_fractional) * kToFloat;
          }
      }

      // --- Feedback ---
      float effect_ramp = fminf(effect_amount / 0.8f, 1.0f);
      const float max_feedback_levels[] = { 0.80f, 0.75f, 0.65f, 0.55f };
      float feedback_amount = effect_ramp * max_feedback_levels[quality];

      static float feedback_energy = 0.0f;
      {
          float energy = fabs(feedback_l) + fabs(feedback_r);
          ONE_POLE(feedback_energy, energy, 0.01f);
          const float energy_ceiling = 1.5f;
          if (feedback_energy > energy_ceiling) {
              feedback_amount *= energy_ceiling / feedback_energy;
          }
      }

      float input_amount = 1.0f - (effect_ramp * 0.85f);
      delayed_l += mix_A * input_amount + feedback_l * feedback_amount;
      delayed_r += mix_B * input_amount + feedback_r * feedback_amount;

      float effective_drive = smear_drive;
      if (pitch_shift_rate > 0.0f) {
          float pitch_ramp = pitch_shift_rate / 0.498f;
          float drive_reduction = pitch_ramp * (1.0f - 1.0f / smear_drive);
          effective_drive = smear_drive * (1.0f - drive_reduction);
      }
      feedback_l = stmlib::SoftLimit(delayed_l * effective_drive);
      feedback_r = stmlib::SoftLimit(delayed_r * effective_drive);

      {
          static float fb_lp_l = 0.0f, fb_lp_r = 0.0f;
          ONE_POLE(fb_lp_l, feedback_l, fb_lp_coeff);
          ONE_POLE(fb_lp_r, feedback_r, fb_lp_coeff);
          feedback_l = fb_lp_l;
          feedback_r = fb_lp_r;
      }

      float output_gain = 1.0f / (1.0f + feedback_amount * 0.6f);
      processed_A = feedback_l * output_gain;
      processed_B = feedback_r * output_gain;

    } else {
      // --- CW: Drift ---
      static float drift_feedback_l = 0.0f, drift_feedback_r = 0.0f;
      static float drift_lfo_phase = 0.0f;
      static float drift_lfo_target = 0.5f;
      static float drift_lfo_current = 0.5f;

      // Base delay: 50-200ms (longer than Smear — creates sustain)
      float base_delay = 4800.0f + effect_amount * 14400.0f;  // 50ms to 200ms

      // Drift LFO: slow random walk for pitch micro-detuning
      drift_lfo_phase += 0.2f * kSamplePeriod;  // 0.2Hz
      if (drift_lfo_phase >= 1.0f) {
          drift_lfo_phase -= 1.0f;
          float new_target = Random::GetFloat();
          const float max_jump = 0.25f;
          if (new_target > drift_lfo_current + max_jump)
              new_target = drift_lfo_current + max_jump;
          else if (new_target < drift_lfo_current - max_jump)
              new_target = drift_lfo_current - max_jump;
          drift_lfo_target = new_target;
      }
      ONE_POLE(drift_lfo_current, drift_lfo_target, 0.0005f);
      float drift_mod = (drift_lfo_current - 0.5f) * 40.0f;  // ±20 samples

      // Stereo spread: drift LFO + fixed detune offset at low quality
      float detune_offset = degradation * 120.0f;  // 0-120 samples (~1.25ms) detuning
      float delay_l = base_delay + drift_mod + detune_offset;
      float delay_r = base_delay - drift_mod - detune_offset;

      const float min_delay = 32.0f;
      delay_l = fmaxf(delay_l, min_delay);
      delay_r = fmaxf(delay_r, min_delay);

      // Read from delay buffer (halve offsets — buffer is decimated in drift mode)
      const float buffer_size_f = (float)kSharedDelaySize;
      float half_delay_l = delay_l * 0.5f;
      float half_delay_r = delay_r * 0.5f;
      float rp_l = (float)shared_write_pos_ - half_delay_l;
      if (rp_l < 0.0f) rp_l += buffer_size_f;
      float rp_r = (float)shared_write_pos_ - half_delay_r;
      if (rp_r < 0.0f) rp_r += buffer_size_f;

      float delayed_l, delayed_r;
      {
          MAKE_INTEGRAL_FRACTIONAL(rp_l);
          int32_t i0 = rp_l_integral;
          int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
          delayed_l = (delay_buffer_[i0].l + (delay_buffer_[i1].l - delay_buffer_[i0].l) * rp_l_fractional) * kToFloat;
      }
      {
          MAKE_INTEGRAL_FRACTIONAL(rp_r);
          int32_t i0 = rp_r_integral;
          int32_t i1 = i0 + 1; if (i1 >= kSharedDelaySize) i1 = 0;
          delayed_r = (delay_buffer_[i0].r + (delay_buffer_[i1].r - delay_buffer_[i0].r) * rp_r_fractional) * kToFloat;
      }

      // Feedback amount: 0.3 at knob=0, up to 0.98 at knob=100%
      float feedback_amount = 0.3f + effect_amount * 0.68f;

      // Progressive lowpass: each recirculation loses highs
      // q3 = bright (0.8), q0 = dark (0.12), knob narrows further
      float filter_coeff = fb_lp_coeff * (1.0f - effect_amount * 0.5f);
      if (filter_coeff < 0.05f) filter_coeff = 0.05f;

      // Mix input with filtered feedback
      float input_amount = 1.0f - effect_amount * 0.7f;  // dry fades as sustain rises
      float fb_l = delayed_l * feedback_amount + mix_A * input_amount;
      float fb_r = delayed_r * feedback_amount + mix_B * input_amount;

      // Apply progressive lowpass on feedback path
      ONE_POLE(drift_feedback_l, fb_l, filter_coeff);
      ONE_POLE(drift_feedback_r, fb_r, filter_coeff);

      // Saturate feedback — low quality drives harder into SoftLimit
      float drift_drive = 1.0f + degradation * 2.0f;  // 1.0 (clean) to 3.0 (warm)
      drift_feedback_l = stmlib::SoftLimit(drift_feedback_l * drift_drive);
      drift_feedback_r = stmlib::SoftLimit(drift_feedback_r * drift_drive);

      processed_A = drift_feedback_l;
      processed_B = drift_feedback_r;
    }

    // --- Write to delay buffer ---
    if (f.algo >= 0.0f) {
      // Drift mode: decimate writes (every 2nd sample) to double buffer range
      drift_decimation_active_ = true;
      drift_decimate_counter_ ^= 1;
      if (drift_decimate_counter_) {
        delay_buffer_[shared_write_pos_].l = Clip16(mix_A * 32768.0f);
        delay_buffer_[shared_write_pos_].r = Clip16(mix_B * 32768.0f);
        shared_write_pos_++;
        if (shared_write_pos_ >= kSharedDelaySize) shared_write_pos_ = 0;
      }
    } else {
      // Smear mode: full-rate writes
      drift_decimation_active_ = false;
      drift_decimate_counter_ = 0;
      delay_buffer_[shared_write_pos_].l = Clip16(mix_A * 32768.0f);
      delay_buffer_[shared_write_pos_].r = Clip16(mix_B * 32768.0f);
      shared_write_pos_++;
      if (shared_write_pos_ >= kSharedDelaySize) shared_write_pos_ = 0;
    }

    MixerWriteOutput(output++, f, processed_A, processed_B);
  }
  MixerFinalize();
}

/* static */
Modulator::XmodFn Modulator::xmod_table_[] = {
  &Modulator::ProcessXmod<ALGORITHM_XFADE, ALGORITHM_FOLD>,
  &Modulator::ProcessXmod<ALGORITHM_FOLD, ALGORITHM_ANALOG_RING_MODULATION>,
  &Modulator::ProcessXmod<
      ALGORITHM_ANALOG_RING_MODULATION, ALGORITHM_DIGITAL_RING_MODULATION>,
  &Modulator::ProcessXmod<ALGORITHM_DIGITAL_RING_MODULATION, ALGORITHM_XOR>,
  &Modulator::ProcessXmod<ALGORITHM_XOR, ALGORITHM_COMPARATOR>,
  &Modulator::ProcessXmod<ALGORITHM_COMPARATOR, ALGORITHM_NOP>,
};

}  // namespace warps
