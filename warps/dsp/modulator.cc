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
  for (int i = 0; i < kSharedDelaySize; ++i) {
    delay_buffer_[i].l = 0;
    delay_buffer_[i].r = 0;
  }

  tape_lp_l_.Init();
  tape_lp_r_.Init();

  lossy_bpf_l_.Init();
  lossy_bpf_r_.Init();
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

// void Modulator::ProcessVocoder(
//     ShortFrame* input,
//     ShortFrame* output,
//     size_t size) {
//   float* carrier = buffer_[0];
//   float* modulator = buffer_[1];
//   float* main_output = buffer_[0];
//   float* aux_output = buffer_[2];

//   if (!parameters_.carrier_shape) {
//     fill(&aux_output[0], &aux_output[size], 0.0f);
//   }

//   // Convert audio inputs to float and apply VCA/saturation (5.8% per channel)
//   short* input_samples = &input->l;
//   for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
//       amplifier_[i].Process(
//           parameters_.channel_drive[i],
//           1.0f,
//           input_samples + i,
//           buffer_[i],
//           aux_output,
//           2,
//           size);
//   }

//   // If necessary, render carrier. Otherwise, sum signals 1 and 2 for aux out.
//   if (parameters_.carrier_shape) {
//     // Scale phase-modulation input.
//     for (size_t i = 0; i < size; ++i) {
//       internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f;
//     }
//     OscillatorShape vocoder_shape = static_cast<OscillatorShape>(
//         parameters_.carrier_shape + 1);
//     OscillatorShape square_shape = static_cast<OscillatorShape>(3);

//     // Outside of the transition zone between the cross-modulation and vocoding
//     // algorithm, we need to render only one of the two oscillators.
//     float carrier_gain = vocoder_oscillator_.Render(
//           vocoder_shape,
//           parameters_.note,
//           internal_modulation_,
//           carrier,
//           size);
    
//     square_oscillator_.Render(
//           square_shape,
//           parameters_.note,
//           non_modulation_,
//           aux_output,
//           size);
//     for (size_t i = 0; i < size; ++i) {
//       carrier[i] = carrier[i] * carrier_gain;
//     }
//   }

//   float release_time = parameters_.modulation_parameter;
//   vocoder_.set_release_time(release_time * (2.0f - release_time));
//   vocoder_.set_formant_shift(parameters_.modulation_algorithm);
//   vocoder_.Process(modulator, carrier, main_output, size);

//   // Convert back to integer and clip.
//   while (size--) {
//     output->l = Clip16(static_cast<int32_t>(*main_output * 32768.0f));
//     output->r = Clip16(static_cast<int32_t>(*aux_output * 16384.0f));
//     ++main_output;
//     ++aux_output;
//     ++output;
//   }
//   previous_parameters_ = parameters_;
// }

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

void Modulator::ProcessDoppler(ShortFrame* input, ShortFrame* output, size_t size) {
  ShortFrame *buffer = delay_buffer_;

  static size_t cursor = 0;
  static float lfo_phase = 0.0f;
  static float distance = 1.0f;
  static float angle = 1.0f;

  float x = previous_parameters_.raw_algorithm * 2.0f - 1.0f;
  float x_end = parameters_.raw_algorithm * 2.0f - 1.0f;

  float y = previous_parameters_.modulation_parameter * 2.0f;
  float y_end = parameters_.modulation_parameter * 2.0f;

  float lfo_freq = parameters_.channel_drive[0]
    * parameters_.channel_drive[0]
    * 50.0f;
  float lfo_amplitude = parameters_.channel_drive[1];

  float step = 1.0f / static_cast<float>(size);
  float x_increment = (x_end - x) * step;
  float y_increment = (y_end - y) * step;

  int8_t shape = parameters_.carrier_shape;

  float atten_factor =
    shape == 0 ? 0.5f :
    shape == 1 ? 4.0f :
    shape == 2 ? 8.0f :
    shape == 3 ? 15.0f : 0;

  float room_size =
    shape == 0 ? 100 :
    shape == 1 ? (DELAY_SIZE - 1) / 10.0f :
    shape == 2 ? (DELAY_SIZE - 1) / 5.0f :
    shape == 3 ? (DELAY_SIZE - 1) / 2.0f : 0;

  while (size--) {

    // write input to buffer
    buffer[cursor].l = input->l;
    buffer[cursor].r = input->r;

    // LFOs
    float sin = Interpolate(lut_sin, lfo_phase, 1024.0f);
    float cos = Interpolate(lut_sin + 256, lfo_phase, 1024.0f);

    float x_lfo = x + sin * lfo_amplitude + 0.05f; // offset avoids discontinuity at 0
    float y_lfo = y + cos  * lfo_amplitude;
    CONSTRAIN(x_lfo, -1.0f, 1.0f);
    CONSTRAIN(y_lfo, -1.0f, 2.0f);

    // compute angular coordinates
    float di = sqrtf(x_lfo * x_lfo + y_lfo * y_lfo); // 0..sqrt(5)
    float an = Interpolate(lut_arcsin, (x_lfo/di + 1.0f) * 0.5f, 256.0f);
    di /= 2.237;		// sqrt(5)

    ONE_POLE(distance, di, 0.001f);
    ONE_POLE(angle, an, 0.001f);

    // compute binaural delay
    float binaural_delay = angle * (96000.0f * 0.0015f); // -1.5ms..1.5ms
    float delay_l = distance * room_size + (angle > 0 ? binaural_delay : 0);
    float delay_r = distance * room_size + (angle < 0 ? -binaural_delay : 0);

    // linear delay interpolation
    MAKE_INTEGRAL_FRACTIONAL(delay_l);
    MAKE_INTEGRAL_FRACTIONAL(delay_r);

    int16_t index_l = cursor - delay_l_integral;
    if (index_l < 0) index_l += DELAY_SIZE;
    int16_t index_r = cursor - delay_r_integral;
    if (index_r < 0) index_r += DELAY_SIZE;

    ShortFrame a_l = buffer[index_l];
    ShortFrame b_l = buffer[index_l == 0 ? DELAY_SIZE - 1 : index_l - 1];
    ShortFrame a_r = buffer[index_r];
    ShortFrame b_r = buffer[index_r == 0 ? DELAY_SIZE - 1 : index_r - 1];

    short s1_l = a_l.l + (b_l.l - a_l.l) * delay_l_fractional;
    short s2_l = a_l.r + (b_l.r - a_l.r) * delay_l_fractional;
    short s1_r = a_r.l + (b_r.l - a_r.l) * delay_r_fractional;
    short s2_r = a_r.r + (b_r.r - a_r.r) * delay_r_fractional;

    // distance attenuation
    float atten = 1.0f + atten_factor * distance * distance;
    s1_l /= atten;
    s2_l /= atten;
    s1_r /= atten;
    s2_r /= atten;

    float fade_in = Interpolate(lut_xfade_in, (angle + 1.0f) / 2.0f, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, (angle + 1.0f) / 2.0f, 256.0f);

    output->l = s2_l * fade_in + s1_l * fade_out;
    output->r = s1_r * fade_in + s2_r * fade_out;

    x += x_increment;
    y += y_increment;
    lfo_phase += lfo_freq / 96000.0f;
    if (lfo_phase > 1.0f) lfo_phase--;
    input++;
    output++;
    cursor = (cursor + 1) % DELAY_SIZE;
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

  case FEATURE_MODE_FREQUENCY_SHIFTER:
    ProcessFreqShifter(input, output, size);
    break;

  case FEATURE_MODE_BITCRUSHER:
    ProcessBitcrusher(input, output, size);
    break;

  case FEATURE_MODE_COMPARATOR:
    Process1<ALGORITHM_COMPARATOR_CHEBYSCHEV>(input, output, size);
    break;

  case FEATURE_MODE_DOPPLER:
    ProcessDoppler(input, output, size);
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

void Modulator::ProcessCrushMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];
  float* aux = buffer_[2];

  // if (!parameters_.carrier_shape) {
  //   fill(&aux[0], &aux[size], 0.0f);
  // }
  short* input_samples = &input->l;
  for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
      amplifier_[i].Process(
          parameters_.channel_drive[i],
          1.0f,
          input_samples + i,
          buffer_[i],
          aux,
          2,
          size);
  }
  if (parameters_.carrier_shape) {
    for (size_t i = 0; i < size; ++i) {
      internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f; 
    }
    OscillatorShape xmod_shape = static_cast<OscillatorShape>(
        parameters_.carrier_shape - 1);
    xmod_oscillator_.Render(
          xmod_shape,
          parameters_.note,
          internal_modulation_,
          in_1,
          size);
    for (size_t i = 0; i < size; ++i) {
      in_1[i] = in_1[i] * kXmodCarrierGain;
    }
  }
  
  ParameterInterpolator effect_interpolator(
      &previous_parameters_.raw_algorithm,
      parameters_.raw_algorithm,
      size);

  ParameterInterpolator crossfade_interpolator(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter,
      size);

  while (size--) {
    float p2 = crossfade_interpolator.Next(); // Crossfader
    float p1_raw =  effect_interpolator.Next(); // Effect Knob
    float x = p1_raw * 2.0f - 1.0f;
    float p1_skewed = (x * x * x + 1.0f) * 0.5f;
    float algo = (p1_skewed * 2.0f) - 1.0f;
    
    float x_1 = *in_1++; 
    float x_2 = *in_2++;

    float fade_in = Interpolate(lut_xfade_in, p2, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, p2, 256.0f);
    
    float mix_A = x_1 * fade_out + x_2 * fade_in; // Main Mix
    float mix_B = x_1 * fade_in + x_2 * fade_out; // Aux (Opposite) Mix

    float effect_amount = fabs(algo);

    // Calculate Fast Crossfade based on absolute amount
    const float xfade_threshold = 0.05f;
    float xfade_amount = 0.0f;
    if (effect_amount < xfade_threshold) {
      xfade_amount = effect_amount / xfade_threshold;
    } else {
      xfade_amount = 1.0f;
    }

    float processed_A = 0.0f;
    float processed_B = 0.0f;
    float out_A = 0.0f;
    float out_B = 0.0f;

    if (algo < 0.0f) {
      // --- CCW: SRR + "JFET" Saturation ---
      // SRR Bitcrusher
      float aliased_A = Xmod<ALGORITHM_SIMPLE_BITCRUSHER>(mix_A, 0.0f, 0.0f, effect_amount);
      float aliased_B = Xmod<ALGORITHM_SIMPLE_BITCRUSHER>(mix_B, 0.0f, 1.0f, effect_amount);

      // "JFET" Saturation
      float drive = 1.0f + effect_amount * 2.0f;
      processed_A = stmlib::SoftLimit(aliased_A * drive);
      processed_B = stmlib::SoftLimit(aliased_B * drive);
    } else {
      // --- CW: Fizzly Chebyshev + Stereo Noise-Modulated Filter ---
      const float stage_switch_threshold = 0.3f;
      float stage_processed_A = 0.0f;
      float stage_processed_B = 0.0f;

      if (effect_amount < stage_switch_threshold) {
        // --- Stage 1: Colouring (Input Drive + Subtle LPF) ---

        // Scale effect_amount from [0.0, 0.3] up to [0.0, 1.0] for control
        float stage1_amount = effect_amount / stage_switch_threshold;
        // Apply curve for smoother start
        float stage1_curved = stage1_amount * stage1_amount;

        // Apply Input Drive (gentle increase in this stage)
        float input_drive = 1.0f + stage1_curved * 1.0f;
        float driven_mix_A = mix_A * input_drive;
        float driven_mix_B = mix_B * input_drive;

        // Apply a very gentle fixed LPF to smooth the drive
        filter_[0].set_f<stmlib::FREQUENCY_FAST>(0.45f); // Keep it fairly open
        filter_[1].set_f<stmlib::FREQUENCY_FAST>(0.45f);
        stage_processed_A = filter_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(driven_mix_A);
        stage_processed_B = filter_[1].Process<stmlib::FILTER_MODE_LOW_PASS>(driven_mix_B);

      } else {
        // --- Stage 2: Full Fizzly Chebyshev ---

        // Scale effect_amount from [0.3, 1.0] up to [0.0, 1.0] for control
        float stage2_amount = (effect_amount - stage_switch_threshold) / (1.0f - stage_switch_threshold);
        // Apply curve for smoother control within this stage
        float stage2_curved = stage2_amount * stage2_amount;

        // --- Smoothed Noise for Modulation ---
        static float noise_mod = 0.0f;
        ONE_POLE(noise_mod, (Random::GetFloat() - 0.5f), 0.01f);

        // --- Input Drive (starts from Stage 1's max and increases) ---
        // Starts at 2x drive (where stage 1 left off) goes up to 3x total
        float input_drive = 2.0f + stage2_curved * 1.0f;
        float driven_mix_A = mix_A * input_drive;
        float driven_mix_B = mix_B * input_drive;

        // --- Modulate Chebyshev Degree ---
        // Degree ramps up across this stage
        float base_degree = stage2_curved * 0.9f;
        float modulated_degree = base_degree + (noise_mod * stage2_curved * 0.15f);
        CONSTRAIN(modulated_degree, 0.0f, 1.0f);
        // Drive param ramps up across this stage
        float drive_param = 0.6f + stage2_curved * 0.1f; // Starts from ~0.5 + 0.1

        // --- Chebyshev Processing ---
        float cheby_A = Xmod<ALGORITHM_CHEBYSCHEV>(driven_mix_A, 0.0f, modulated_degree, drive_param);
        float cheby_B = Xmod<ALGORITHM_CHEBYSCHEV>(driven_mix_B, 0.0f, modulated_degree, drive_param);

        // --- Post-Filter with Noise Modulation ---
        // Filter starts fairly open and gets darker
        float base_lpf_cutoff = 0.45f - stage2_curved * 0.20f; // Start near Stage 1's LPF
        float cutoff_mod_depth = stage2_curved * 0.08f;

        float cutoff_A = base_lpf_cutoff + (noise_mod * cutoff_mod_depth);
        CONSTRAIN(cutoff_A, 0.01f, 0.49f);
        float cutoff_B = base_lpf_cutoff - (noise_mod * cutoff_mod_depth);
        CONSTRAIN(cutoff_B, 0.01f, 0.49f);

        // Use filter_[0] and filter_[1] for the post-filter in this stage
        filter_[0].set_f<stmlib::FREQUENCY_FAST>(cutoff_A);
        filter_[1].set_f<stmlib::FREQUENCY_FAST>(cutoff_B);
        float filtered_A = filter_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(cheby_A);
        float filtered_B = filter_[1].Process<stmlib::FILTER_MODE_LOW_PASS>(cheby_B);

        // --- Gain Compensation ---
        float gain_comp = 1.0f - (stage2_curved * 0.15f);
        stage_processed_A = filtered_A * gain_comp;
        stage_processed_B = filtered_B * gain_comp;
      }

      processed_A = stage_processed_A;
      processed_B = stage_processed_B;

    } // End CW block

    float fade_wet = Interpolate(lut_xfade_in, xfade_amount, 256.0f);
    float fade_dry = Interpolate(lut_xfade_out, xfade_amount, 256.0f);

    out_A = mix_A * fade_dry + processed_A * fade_wet;
    out_B = mix_B * fade_dry + processed_B * fade_wet;
    
    output->l = Clip16(out_A * 32768.0f);
    output->r = Clip16(out_B * 32768.0f);

    output++;
  }
  
  previous_parameters_.raw_algorithm = parameters_.raw_algorithm;
  previous_parameters_.modulation_parameter = parameters_.modulation_parameter;
}

void Modulator::ProcessCassetteMixer(ShortFrame* input, ShortFrame* output, size_t size) {
  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];
  float* aux = buffer_[2];

  ParameterInterpolator effect_interpolator(
      &previous_parameters_.raw_algorithm,
      parameters_.raw_algorithm,
      size);

  ParameterInterpolator crossfade_interpolator(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter,
      size);

  // if (!parameters_.carrier_shape) {
  //   fill(&aux[0], &aux[size], 0.0f);
  // }
  short* input_samples = &input->l;
  for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
      amplifier_[i].Process(
          parameters_.channel_drive[i],
          1.0f,
          input_samples + i,
          buffer_[i],
          aux,
          2,
          size);
  }
  if (parameters_.carrier_shape) {
    for (size_t i = 0; i < size; ++i) {
      internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f;
    }
    OscillatorShape xmod_shape = static_cast<OscillatorShape>(
        parameters_.carrier_shape - 1);
    xmod_oscillator_.Render(
          xmod_shape,
          parameters_.note,
          internal_modulation_,
          in_1,
          size);
    for (size_t i = 0; i < size; ++i) {
      in_1[i] = in_1[i] * kXmodCarrierGain;
    }
  }

  float effect_target = effect_interpolator.Next(); 
  float x_target = effect_target * 2.0f - 1.0f;
  float effect_skewed_target = (x_target * x_target * x_target + 1.0f) * 0.5f;
  float algo = (effect_skewed_target * 2.0f) - 1.0f;
  float effect_amount = fabs(algo);
  float effect_amount_sqrtf = sqrtf(effect_amount);

  if (algo < 0.0f) {
    // --- SET CASSETTE FILTERS ---
    filter_[4].set_f<stmlib::FREQUENCY_FAST>(0.4f);
    filter_[5].set_f<stmlib::FREQUENCY_FAST>(0.4f);
    // filter_[2].set_f<stmlib::FREQUENCY_FAST>(0.25f); // Hiss filter L // temporarly disabled
    filter_[3].set_f<stmlib::FREQUENCY_DIRTY>(0.25f); // Hiss filter R // try dirty filter instead of fast
    float target_filter_cutoff = 0.05f + (1.0f - effect_amount_sqrtf) * 0.45f;
    tape_lp_l_.set_f<stmlib::FREQUENCY_FAST>(target_filter_cutoff);
    tape_lp_r_.set_f<stmlib::FREQUENCY_FAST>(target_filter_cutoff);
  } else {
    // --- SET VHS FILTERS ---
    //tilt filters
    filter_[0].set_f<stmlib::FREQUENCY_FAST>(0.02f); // L-LPF
    filter_[1].set_f<stmlib::FREQUENCY_FAST>(0.02f); // L-HPF
    filter_[2].set_f<stmlib::FREQUENCY_FAST>(0.02f); // R-LPF
    filter_[3].set_f<stmlib::FREQUENCY_FAST>(0.02f); // R-HPF

    //snow filters
    float age_mid   = smoothstep(0.3f, 0.8f, effect_amount);
    float age_chewed = smoothstep(0.7f, 1.0f, effect_amount);
    float low_cutoff  = 3000.0f + 2000.0f * age_mid;
    float high_cutoff = 4000.0f + 4000.0f * age_chewed;

    // Try frequency dirty instead of fast
    filter_[4].set_f<stmlib::FREQUENCY_DIRTY>(low_cutoff / 96000.0f);  // L-LPF
    filter_[5].set_f<stmlib::FREQUENCY_DIRTY>(high_cutoff / 96000.0f); // L-HPF
    filter_[6].set_f<stmlib::FREQUENCY_DIRTY>(low_cutoff / 96000.0f);  // R-LPF
    filter_[7].set_f<stmlib::FREQUENCY_DIRTY>(high_cutoff / 96000.0f); // R-HPF

    // hiss filters
    tape_lp_l_.set_f<stmlib::FREQUENCY_DIRTY>(0.05f); 
    tape_lp_r_.set_f<stmlib::FREQUENCY_DIRTY>(0.25f);
  }

  while (size--) {
    float p2 = crossfade_interpolator.Next(); // Crossfader
    // float p1_raw =  effect_interpolator.Next(); // Effect Knob
    // float x = p1_raw * 2.0f - 1.0f;
    // float p1_skewed = (x * x * x + 1.0f) * 0.5f;
    // float algo = (p1_skewed * 2.0f) - 1.0f;

    float x_1 = *in_1++;
    float x_2 = *in_2++;

    float fade_in = Interpolate(lut_xfade_in, p2, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, p2, 256.0f);

    float mix_A = x_1 * fade_out + x_2 * fade_in; // Main Mix
    float mix_B = x_1 * fade_in + x_2 * fade_out; // Aux (Opposite) Mix
    
    const float xfade_threshold = 0.05f;
    float xfade_amount = 0.0f;
    if (effect_amount < xfade_threshold) {
      xfade_amount = effect_amount / xfade_threshold;
    } else {
      xfade_amount = 1.0f;
    }

    float processed_A = 0.0f;
    float processed_B = 0.0f;

    float out_A = 0.0f;
    float out_B = 0.0f;

    // Hiss noise generator for both modes
    static uint32_t hiss_rng_state_ = 0;
    hiss_rng_state_ = 1664525L * hiss_rng_state_ + 1013904223L;

    if (algo < 0.0f) {
      // --- CCW: Cassette Emulation ---
      // static float random_drift_lfo_phase_ = 0.0f;
      int random_drift_counter_ = 0;

      static float flutter_lfo_phase = 0.0f;
      static float wow_lfo_phase = 0.0f;
      static float random_drift_target_val_ = 0.5f;
      static float random_drift_current_val_ = 0.5f;
      static float hiss_envelope_ = 0.0f;
      static float allpass_z1_ = 0.0f;
      static float mangle_read_pos_f = 0.0f;

      const float mangle_threshold = 0.86f;
      const float kSafeDistance = 0.5f;

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
      wow_lfo_phase += wow_freq / 96000.0f;
      if (wow_lfo_phase >= 1.0f) wow_lfo_phase -= 1.0f;

      flutter_lfo_phase += flutter_freq / 96000.0f;
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

      int wow_i_r = (wow_i + (lut_size >> 3)) & (lut_size - 1);
      float wow_mod_r = lut_sin[wow_i_r];

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
      float total_mod_l = (wow_mod + flutter_mod * flutter_scale) * 0.5f * effect_amount_sqrtf;
      float total_mod_r = (wow_mod_r + flutter_mod_r * flutter_scale) * 0.5f * effect_amount_sqrtf;

      // 8. Compute final delay offsets
      const float kFixedTapeDelay = 1.0f;
      float delay_offset_l = total_mod_l * 3.0f;
      float delay_offset_r = total_mod_r * 3.0f;

      float base_read_pos = shared_write_pos_ - kFixedTapeDelay;

      if (effect_amount >= mangle_threshold) {
          // --- "TAPE MANGLE" (PITCH SHIFT) ---
          float mangle_amount = (effect_amount - mangle_threshold) / (1.0f - mangle_threshold);
          float read_speed = (mangle_amount < 0.8f) ? 0.5f : 0.25f;

          mangle_read_pos_f += read_speed;
          //replace fmodf with manual wrap
          if (mangle_read_pos_f >= (float)kSharedDelaySize)
              mangle_read_pos_f -= (float)kSharedDelaySize;

          // mangle_read_pos_f = fmodf(mangle_read_pos_f, (float)kSharedDelaySize);

          //replace distance calculation with manual wrap
          float distance = (float)shared_write_pos_ - mangle_read_pos_f + (float)kSharedDelaySize;
          if (distance < kSafeDistance)
              mangle_read_pos_f = (float)shared_write_pos_ - kSafeDistance;

          base_read_pos = mangle_read_pos_f;
      } else {
          // --- ORIGINAL WOW/FLUTTER (effect_amount < 0.88) ---
          base_read_pos = shared_write_pos_ - kFixedTapeDelay;
          mangle_read_pos_f = base_read_pos;
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
          const float kToFloat = 1.0f / 32768.0f;
          float x0 = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
          float x1 = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].l) * kToFloat;
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
          const float kToFloat = 1.0f / 32768.0f;
          float x0 = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
          float x1 = static_cast<float>(delay_buffer_[(index_integral + 1) % kSharedDelaySize].r) * kToFloat;
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

      // --- Saturation ---
      float shaped = effect_amount * effect_amount;
      float drive = 1.0f + shaped * 4.0f;
      float offset = shaped * 0.15f;
      float sat_l = stmlib::SoftLimit((delayed_l * drive) + offset);
      float sat_r = stmlib::SoftLimit((delayed_r * drive) + offset);

      // --- De-emphasis ---
      sat_l = filter_[4].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_l);
      sat_r = filter_[5].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_r);

      // --- Filtering (Tape Bandwidth) ---
      float filtered_l = filter_[6].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_l);
      float filtered_r = filter_[7].Process<stmlib::FILTER_MODE_LOW_PASS>(sat_r);

      // --- Hiss (Dynamic) ---
      const float hiss_threshold = 0.65f;
      float hiss_amount = (effect_amount > hiss_threshold) ? hiss_threshold : effect_amount;
      float base_hiss_level = hiss_amount * 0.03f;
      float dynamic_hiss_level = hiss_envelope_ * 2.0f * hiss_amount * 0.4f; 
      float hiss_level = (base_hiss_level + dynamic_hiss_level);

      float hiss_raw_l = static_cast<float>(hiss_rng_state_) / 4294967296.0f;
      float hiss_l = (hiss_raw_l - 0.5f) * hiss_level;

      float hiss_filtered_l = filter_[3].Process<stmlib::FILTER_MODE_HIGH_PASS>(hiss_l);
      // float hiss_filtered_r = filter_[2].Process<stmlib::FILTER_MODE_HIGH_PASS>(hiss_r); 

      processed_A = filtered_l + hiss_filtered_l;
      processed_B = filtered_r - hiss_filtered_l; // Mono hiss for cpu? Now we invert it for cheap fake stereo
    } else {
      // --- CW: VHS Emulation ---

      //same as above can we optimize by moving out of the loop?
      float age_mid   = smoothstep(0.3f, 0.8f, effect_amount);
      float age_chewed = smoothstep(0.7f, 1.0f, effect_amount);
      float dropout_base_chance;
      float dropout_max_length;
      float dropout_gain_min;

      if (effect_amount < 0.1f) { // Fresh
          dropout_base_chance = 0.01f;
          dropout_max_length  = 0.010f;
          dropout_gain_min    = 0.6f;
      } else if (effect_amount < 0.6f) { // Mid-Age
          dropout_base_chance = 0.05f;
          dropout_max_length  = 0.035f;
          dropout_gain_min    = 0.4f;
      } else { // Chewed
          dropout_base_chance = 0.2f;
          dropout_max_length  = 0.070f;
          dropout_gain_min    = 0.2f;
      }
      
      // --- State Variables ---
      static float dropout_check_timer = 0.1f;
      static float dropout_duration_timer = 0.0f;
      static float hum_lfo_phase_ = 0.0f;
      
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

      // VHS-style mixed noise
      float vhs_snow_L =
      low_noise_L  * (0.2f + age_mid * 0.3f)
      + noise_mid    * (0.6f + age_chewed * 0.3f)
      + high_noise_L * (0.2f * age_chewed);

      float vhs_snow_R =
      low_noise_R  * (0.2f + age_mid * 0.3f)
      + noise_mid    * (0.6f + age_chewed * 0.3f)
      + high_noise_R * (0.2f * age_chewed);

      // --- Noise Amount / Leveling ---
      float noise_level = 0.02f + age_mid * 0.05f + age_chewed * 0.15f;
      float snow_boost  = (effect_amount > 0.95f) ? (effect_amount - 0.95f) * 8.0f : 0.0f;
      float noise_amount = (noise_level + snow_boost) * (0.8f + breathing * 1.2f);  // breathing mod

      // --- Dropout Logic (gets chaotic at the end) ---
      float dropout_gain = 1.0f;
      float dropout_tilt_mod = 0.0f; 

      if (dropout_duration_timer > 0.0f) {
        // --- We ARE currently dropping out ---
        dropout_duration_timer -= 1.0f / 96000.0f;
        dropout_gain = dropout_gain_min;
        dropout_tilt_mod = 1.0f;
      } else {
        // --- We are NOT dropping out, check if we should ---
        dropout_check_timer -= 1.0f / 96000.0f;
        if (dropout_check_timer <= 0.0f) {
          if (Random::GetFloat() < dropout_base_chance) {
              dropout_duration_timer = 0.001f + Random::GetFloat() * dropout_max_length;
          }
          dropout_check_timer = 0.01f + (1.0f - effect_amount) * 0.05f;
        }
      }

      // Apply dropout gain
      float signal_A = mix_A * dropout_gain;
      float signal_B = mix_B * dropout_gain;

      // --- Tape Saturation Stage ---
      float drive = 1.0f + effect_amount * 2.0f;
      signal_A = stmlib::SoftLimit(signal_A * drive);
      signal_B = stmlib::SoftLimit(signal_B * drive);

      // --- Tilt Filter ---
      float tilt_lpf_A = filter_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(signal_A);
      float tilt_hpf_A = filter_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(signal_A);
      float tilt_lpf_B = filter_[2].Process<stmlib::FILTER_MODE_LOW_PASS>(signal_B);
      float tilt_hpf_B = filter_[3].Process<stmlib::FILTER_MODE_HIGH_PASS>(signal_B);

      float tilt_mix = 0.5f + (effect_amount * 0.5f);

      if (dropout_tilt_mod > 0.5f) {
        tilt_mix = 1.0f; // Force dark
      }

      float filtered_A = tilt_hpf_A * (1.0f - tilt_mix) + tilt_lpf_A * tilt_mix;
      float filtered_B = tilt_hpf_B * (1.0f - tilt_mix) + tilt_lpf_B * tilt_mix;

      // --- 60Hz Hum ---
      float random_val = static_cast<float>((hiss_rng_state_ >> 9) & 0x7FFFFF) / 8388607.0f;
      static float hum_instability = 0.0f;
      float target = age_chewed * (random_val - 0.5f) * 0.02f;
      ONE_POLE(hum_instability, target, 0.0005f);

      float hum_freq = 60.0f + hum_instability; 
      hum_lfo_phase_ += hum_freq / 96000.0f;
      if (hum_lfo_phase_ >= 1.0f) hum_lfo_phase_ -= 1.0f;
      float hum = Interpolate(lut_sin, hum_lfo_phase_, 1024.0f);
      hum = hum * (1.0f + 0.25f * hum);  // adds a bit of 2nd harmonic distortion
      float hum_amount = effect_amount * 0.03f;
      
      // --- Add Noise & Hum ---
      processed_A = filtered_A + (vhs_snow_L * noise_amount * 0.04f) + (hum * hum_amount);
      processed_B = filtered_B + (vhs_snow_R * noise_amount * 0.04f) + (hum * hum_amount);
    }

    float fade_wet = Interpolate(lut_xfade_in, xfade_amount, 256.0f);
    float fade_dry = Interpolate(lut_xfade_out, xfade_amount, 256.0f);

    out_A = mix_A * fade_dry + processed_A * fade_wet;
    out_B = mix_B * fade_dry + processed_B * fade_wet;

    delay_buffer_[shared_write_pos_].l = Clip16(mix_A * 32768.0f);
    delay_buffer_[shared_write_pos_].r = Clip16(mix_B * 32768.0f);
    shared_write_pos_ = (shared_write_pos_ + 1) % kSharedDelaySize;

    output->l = Clip16(out_A * 32768.0f);
    output->r = Clip16(out_B * 32768.0f);

    output++;
  }

  previous_parameters_.raw_algorithm = parameters_.raw_algorithm;
  previous_parameters_.modulation_parameter = parameters_.modulation_parameter;
}

void Modulator::ProcessLossyMixer(ShortFrame* input, ShortFrame* output, size_t size) {
    float* in_1 = buffer_[0];
    float* in_2 = buffer_[1];
    float* aux = buffer_[2];


  ParameterInterpolator effect_interpolator(
       &previous_parameters_.raw_algorithm,
       parameters_.raw_algorithm,
       size);

  ParameterInterpolator crossfade_interpolator(
       &previous_parameters_.modulation_parameter,
       parameters_.modulation_parameter,
       size);

    // if (!parameters_.carrier_shape) {
    //   fill(&aux[0], &aux[size], 0.0f);
    // }
    short* input_samples = &input->l;
    for (int32_t i = parameters_.carrier_shape ? 1 : 0; i < 2; ++i) {
      amplifier_[i].Process(
          parameters_.channel_drive[i],
          1.0f,
          input_samples + i,
          buffer_[i],
          aux,
          2,
          size);
    }

    if (parameters_.carrier_shape) {
      for (size_t i = 0; i < size; ++i) {
        internal_modulation_[i] = static_cast<float>(input[i].l) / 32768.0f;
      }
      OscillatorShape xmod_shape = static_cast<OscillatorShape>(
          parameters_.carrier_shape - 1);
      xmod_oscillator_.Render(
            xmod_shape,
            parameters_.note,
            internal_modulation_,
            in_1,
            size);
      for (size_t i = 0; i < size; ++i) {
        in_1[i] = in_1[i] * kXmodCarrierGain;
      }
    }

  while (size--) {
    bool write_to_buffer = true;

    float effect_target = effect_interpolator.Next(); // Effect Knob
    float x_target = effect_target * 2.0f - 1.0f;
    float effect_skewed_target = (x_target * x_target * x_target + 1.0f) * 0.5f;
    float algo = (effect_skewed_target * 2.0f) - 1.0f;
    float effect_amount = fabs(algo);
    float effect_amount_sqrtf = sqrtf(effect_amount);
    
    // --- Clock to write every other sample ---
    static bool write_clock = false;
    write_clock = !write_clock;

    float crossfade = crossfade_interpolator.Next(); // Crossfader

    float x_1 = *in_1++;
    float x_2 = *in_2++;

    float fade_in = Interpolate(lut_xfade_in, crossfade, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, crossfade, 256.0f);

    float mix_A = x_1 * fade_out + x_2 * fade_in; // Main Mix
    float mix_B = x_1 * fade_in + x_2 * fade_out; // Aux (Opposite) Mix

    const float xfade_threshold = 0.05f;
    float xfade_amount = 0.0f;
    if (effect_amount < xfade_threshold) {
      xfade_amount = effect_amount / xfade_threshold;
    } else {
      xfade_amount = 1.0f;
    }

    float processed_A = 0.0f;
    float processed_B = 0.0f;
    float out_A = 0.0f;
    float out_B = 0.0f;

      if (algo < 0.0f) {
        // --- CCW: Granular Smear / Chorus ---
        static float feedback_l = 0.0f, feedback_r = 0.0f;
        static float smear_lfo_phase_ = 0.0f;
        static float smear_lfo_target_val_ = 0.5f;
        static float smear_lfo_current_val_ = 0.5f;
        static float chorus_lfo_phase_ = 0.0f;
        static float pitch_offset_l_ = 0.0f;
        static float pitch_offset_r_ = 0.0f;

        static int lfo_counter_ = 0;
        static float smear_mod_held_ = 0.0f;
        static float chorus_mod_held_ = 0.0f;

        // Calculate Target Delay
        float base_delay_samples = 2880.0f + effect_amount * 8640.0f;
        const int kLfoUpdateRate = 48; // Update LFOs every 48 samples
        if (lfo_counter_ == 0) {
          // Update Smear LFO
          smear_lfo_phase_ += (0.2f * kLfoUpdateRate) / 96000.0f; // Scale increment
          if (smear_lfo_phase_ >= 1.0f) {
              smear_lfo_phase_ -= 1.0f;
              // smear_lfo_target_val_ = Random::GetFloat();

              // float new_target = Random::GetFloat();
              // smear_lfo_target_val_ = 0.95f * smear_lfo_target_val_ + 0.05f * new_target;
              smear_lfo_target_val_ += (Random::GetFloat() - 0.5f) * 0.1f; 
              smear_lfo_target_val_ = fminf(1.0f, fmaxf(0.0f, smear_lfo_target_val_));
          }
          smear_mod_held_ = (smear_lfo_current_val_ - 0.5f) * 2.0f;

          // Update the Chorus LFO
          chorus_lfo_phase_ += (0.3f * kLfoUpdateRate) / 96000.0f; // Scale increment
          if (chorus_lfo_phase_ >= 1.0f) chorus_lfo_phase_ -= 1.0f;
            chorus_mod_held_ = Interpolate(lut_sin, chorus_lfo_phase_, 1024.0f);
        }

        // We still need to run the smoothing filter every sample
        ONE_POLE(smear_lfo_current_val_, smear_lfo_target_val_, 0.00001f);

        // Increment counter
        lfo_counter_ = (lfo_counter_ + 1) % kLfoUpdateRate;

        // Use the "held" (cached) LFO values
        float smear_mod = smear_mod_held_ * 0.5f;  // halve range
        float chorus_mod = chorus_mod_held_;
        // Calculate Final Read Positions
        float smear_mod_amount = effect_amount * 1920.0f; 
        float stereo_offset = (100.0f + chorus_mod * 100.0f) * effect_amount;
        
        float delay_l_samples = base_delay_samples - stereo_offset + (smear_mod * smear_mod_amount);
        float delay_r_samples = base_delay_samples + stereo_offset - (smear_mod * smear_mod_amount);
        

        //crackle fix?
        const float min_delay = 32.0f;
        delay_l_samples = fmaxf(delay_l_samples, min_delay);
        delay_r_samples = fmaxf(delay_r_samples, min_delay);

        const float pitch_threshold = 0.8f;

        // Calculate the ramp. This will be negative or zero if we're below the threshold.
        float pitch_ramp = (effect_amount - pitch_threshold) / (1.0f - pitch_threshold);
        
        // Use fmaxf to clamp the ramp at 0. This is "branchless" and fast.
        // If ramp was negative, it becomes 0. If it was positive, it stays.
        pitch_ramp = fmaxf(0.0f, pitch_ramp); 

        // Now pitch_shift_rate is 0.0 if ramp is 0, or a positive value.
        float pitch_shift_rate = 0.498f * pitch_ramp;
          
        pitch_offset_l_ += pitch_shift_rate;
        pitch_offset_r_ += pitch_shift_rate;
        const float buffer_size_f = (float)kSharedDelaySize;

        // if (pitch_offset_l_ >= buffer_size_f) pitch_offset_l_ -= buffer_size_f;
        // else if (pitch_offset_l_ < 0.0f) pitch_offset_l_ += buffer_size_f;

        // if (pitch_offset_r_ >= buffer_size_f) pitch_offset_r_ -= buffer_size_f;
        // else if (pitch_offset_r_ < 0.0f) pitch_offset_r_ += buffer_size_f;

        pitch_offset_l_ -= buffer_size_f * floorf(pitch_offset_l_ / buffer_size_f);
        pitch_offset_r_ -= buffer_size_f * floorf(pitch_offset_r_ / buffer_size_f);
        
        // Apply the pitch offset to the read position
        float read_pos_l_to_use = (float)shared_write_pos_ - delay_l_samples + pitch_offset_l_;
        float read_pos_r_to_use = (float)shared_write_pos_ - delay_r_samples + pitch_offset_r_;
        

        // --- Read audio (L Channel) ---
        float delayed_l;
        {
            float read_pos = read_pos_l_to_use;
            if (read_pos >= kSharedDelaySize) read_pos -= kSharedDelaySize;
            else if (read_pos < 0.0f) read_pos += kSharedDelaySize;

            read_pos = roundf(read_pos * 10000.0f) * 0.0001f;
            MAKE_INTEGRAL_FRACTIONAL(read_pos);
            int32_t index_integral = read_pos_integral;
            float index_fractional = read_pos_fractional;

            const float kToFloat = 1.0f / 32768.0f;
            const int mask = kSharedDelaySize - 1;

            const float x0 = delay_buffer_[ index_integral & mask ].l * kToFloat;
            const float x1 = delay_buffer_[ (index_integral + 1) & mask ].l * kToFloat;

            // Linear interpolation
            delayed_l = x0 + (x1 - x0) * index_fractional;
        }

        // --- Read audio (R Channel) ---
        float delayed_r;
        {
            float read_pos = read_pos_r_to_use;
            if (read_pos >= kSharedDelaySize) read_pos -= kSharedDelaySize;
            else if (read_pos < 0.0f) read_pos += kSharedDelaySize;

            read_pos = roundf(read_pos * 10000.0f) * 0.0001f;
            MAKE_INTEGRAL_FRACTIONAL(read_pos);
            int32_t index_integral = read_pos_integral;
            float index_fractional = read_pos_fractional;

            const float kToFloat = 1.0f / 32768.0f;
            const int mask = kSharedDelaySize - 1;

            const float x0 = delay_buffer_[ index_integral & mask ].r * kToFloat;
            const float x1 = delay_buffer_[ (index_integral + 1) & mask ].r * kToFloat;

            // Linear interpolation
            delayed_r = x0 + (x1 - x0) * index_fractional;
        }


        //HERMITE INTERPOLATION READS
        // // --- Read audio (L Channel) ---
        // float delayed_l;
        // {
        //     float read_pos = read_pos_l_to_use;
        //     if (read_pos >= kSharedDelaySize) read_pos -= kSharedDelaySize;
        //     else if (read_pos < 0.0f) read_pos += kSharedDelaySize;
        //     MAKE_INTEGRAL_FRACTIONAL(read_pos);
        //     int32_t index_integral = read_pos_integral;
        //     float index_fractional = read_pos_fractional;
        //     const float kToFloat = 1.0f / 32768.0f;
        //     const int mask = kSharedDelaySize - 1;
        //     const float xm1 = delay_buffer_[(index_integral - 1) & mask].l * kToFloat;
        //     const float x0  = delay_buffer_[ index_integral & mask].l * kToFloat;
        //     const float x1  = delay_buffer_[(index_integral + 1) & mask].l * kToFloat;
        //     const float x2  = delay_buffer_[(index_integral + 2) & mask].l * kToFloat;

        //     const float c = (x1 - xm1) * 0.5f;
        //     const float v = x0 - x1;
        //     const float w = c + v;
        //     const float a = w + v + (x2 - x0) * 0.5f;
        //     const float b_neg = w + a;

        //     const float t = index_fractional;
        //     delayed_l = (((a * t - b_neg) * t + c) * t + x0);
        // }

        // // --- Read audio (R Channel) ---
        // float delayed_r;
        // {
        //     float read_pos = read_pos_r_to_use;
        //     if (read_pos >= kSharedDelaySize) read_pos -= kSharedDelaySize;
        //     else if (read_pos < 0.0f) read_pos += kSharedDelaySize;
        //     MAKE_INTEGRAL_FRACTIONAL(read_pos);
        //     int32_t index_integral = read_pos_integral;
        //     float index_fractional = read_pos_fractional;
        //     const float kToFloat = 1.0f / 32768.0f;
        //     const int mask = kSharedDelaySize - 1;

        //     const float xm1 = delay_buffer_[(index_integral - 1) & mask].r * kToFloat;
        //     const float x0  = delay_buffer_[ index_integral & mask].r * kToFloat;
        //     const float x1  = delay_buffer_[(index_integral + 1) & mask].r * kToFloat;
        //     const float x2  = delay_buffer_[(index_integral + 2) & mask].r * kToFloat;

        //     const float c = (x1 - xm1) * 0.5f;
        //     const float v = x0 - x1;
        //     const float w = c + v;
        //     const float a = w + v + (x2 - x0) * 0.5f;
        //     const float b_neg = w + a;

        //     const float t = index_fractional;
        //     delayed_r = (((a * t - b_neg) * t + c) * t + x0);
        // }

        // --- "Smear Magic" (Internal Feedback) ---
        float effect_ramp = fminf(effect_amount / pitch_threshold, 1.0f);

        // Feedback now maxes out at 0.75 *at 80% knob*
        const float max_feedback_level = 0.75f; // Tune this max level
        float feedback_amount = effect_ramp * max_feedback_level;
        
        // Input amount now hits its minimum *at 80% knob* and stays there
        float input_amount = 1.0f - (effect_ramp * 0.85f); 
        
        float feedback_in_l = mix_A * input_amount + (feedback_l * feedback_amount);
        float feedback_in_r = mix_B * input_amount + (feedback_r * feedback_amount);
        
        delayed_l += feedback_in_l;
        delayed_r += feedback_in_r;
        
        feedback_l = stmlib::SoftLimit(delayed_l);
        feedback_r = stmlib::SoftLimit(delayed_r);

        // --- Set Output ---
        processed_A = feedback_l;
        processed_B = feedback_r;
      } else {
        // --- CW: Glitches ---
        static int decimation_counter_ = 0;
        static float latched_l_ = 0.0f;
        static float latched_r_ = 0.0f;
        static bool decimator_needs_reset_ = true;

        // 32kHz Decimator for glitch stability
        decimation_counter_ = (decimation_counter_ + 1) % 3;
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
        static float loop_read_speed = 0.5f;
        static bool force_loop_needs_capture = true;
        const float force_loop_threshold = 0.99f;
        static float force_loop_crossfade_center_ = 0.5f;

        const float silence_threshold = 0.0001f;
        bool is_silent = (fabs(mix_A) < silence_threshold) && (fabs(mix_B) < silence_threshold);

        glitch_timer -= 1.0f / 96000.0f;

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
                force_loop_crossfade_center_ = crossfade;
                force_loop_needs_capture = false;
            }

            float delta = crossfade - force_loop_crossfade_center_;
            const float speed_sensitivity = 2.0f;
            loop_read_speed = 0.5f + delta * speed_sensitivity;
        } else {
            // --- NORMAL PROBABILISTIC ZONE (0% to 99%) ---
            
            force_loop_needs_capture = true; 
            
            if (currently_glitching) {
                glitch_hold_duration -= 1.0f / 96000.0f;
                if (glitch_hold_duration <= 0.0f || is_silent) {
                    currently_glitching = false;
                    pitch_glitching = false;
                    micro_looping = false;
                    glitch_timer = 0.005f + Random::GetFloat() * 0.01f;
                }
            } else {
                if (glitch_timer <= 0.0f) {
                  
                  float glitch_chance = sqrtf(effect_amount) * 0.25f;
                  
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
                        loop_read_speed = 0.5f; 
                        
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
                { /* (Hermite code) */
                    int32_t index_integral = glitch_read_pos_f_integral;
                    float index_fractional = glitch_read_pos_f_fractional;
                    const float kToFloat = 1.0f / 32768.0f;
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

            } else if (micro_looping) {
                // --- MICRO-LOOP ---
                loop_read_pos_f += loop_read_speed; 
                
                float loop_end_pos = loop_start_pos_f + loop_length_f;
                
                if (loop_read_speed > 0.0f && loop_read_pos_f >= loop_end_pos) {
                    loop_read_pos_f -= loop_length_f;
                } else if (loop_read_speed < 0.0f && loop_read_pos_f < loop_start_pos_f) {
                    loop_read_pos_f += loop_length_f;
                }
                
                float read_pos_for_hermite = fmodf(loop_read_pos_f, (float)kSharedDelaySize);
                if (read_pos_for_hermite < 0.0f) {
                    read_pos_for_hermite += (float)kSharedDelaySize;
                }

                MAKE_INTEGRAL_FRACTIONAL(read_pos_for_hermite);
                { /* (Hermite code) */
                    int32_t index_integral = read_pos_for_hermite_integral;
                    float index_fractional = read_pos_for_hermite_fractional;
                    const float kToFloat = 1.0f / 32768.0f;
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
        
        // --- "Birdie" ---
        birdie_lfo_phase_ += (0.5f + effect_amount * 3.0f) / 96000.0f;
        if (birdie_lfo_phase_ >= 1.0f) {
            birdie_lfo_phase_ -= 1.0f;
            birdie_lfo_target_val_ = Random::GetFloat();
        }
        ONE_POLE(birdie_lfo_current_val_, birdie_lfo_target_val_, 0.001f);

        const int kBirdieUpdateRate = 32; 
        if (birdie_update_counter_ == 0) {
          float bpf_cutoff = 0.1f + (birdie_lfo_current_val_ * 0.2f);
          float bpf_resonance = 1.0f + effect_amount_sqrtf * 30.0f;
          
          lossy_bpf_l_.set_f_q<stmlib::FREQUENCY_FAST>(bpf_cutoff, bpf_resonance);
          lossy_bpf_r_.set_f_q<stmlib::FREQUENCY_FAST>(bpf_cutoff, bpf_resonance);
        }

        // Increment and wrap the counter every sample
        birdie_update_counter_ = (birdie_update_counter_ + 1) % kBirdieUpdateRate;
        
        float base_signal_l = (currently_glitching) ? glitch_l : latched_l_;
        float base_signal_r = (currently_glitching) ? glitch_r : latched_r_;

        float birdie_l = lossy_bpf_l_.Process<stmlib::FILTER_MODE_BAND_PASS>(base_signal_l);
        float birdie_r = lossy_bpf_r_.Process<stmlib::FILTER_MODE_BAND_PASS>(base_signal_r);
        
        // --- Final Mix ---
        float birdie_mix = effect_amount_sqrtf * 0.5f;
        
        processed_A = base_signal_l * (1.0f - birdie_mix) + birdie_l * birdie_mix;
        processed_B = base_signal_r * (1.0f - birdie_mix) + birdie_r * birdie_mix;
      }

    // --- FINAL MIX ---
    float fade_wet = Interpolate(lut_xfade_in, xfade_amount, 256.0f);
    float fade_dry = Interpolate(lut_xfade_out, xfade_amount, 256.0f);

    out_A = mix_A * fade_dry + processed_A * fade_wet;
    out_B = mix_B * fade_dry + processed_B * fade_wet;
    
    if (write_to_buffer) {
        static int16_t last_written_l = 0;
        static int16_t last_written_r = 0;

        if (write_clock) {
            // heavy math happens only on write_clock samples
            last_written_l = Clip16(mix_A * 32768.0f);
            last_written_r = Clip16(mix_B * 32768.0f);
        }

        // lightweight store (just copy) each sample
        delay_buffer_[shared_write_pos_].l = last_written_l;
        delay_buffer_[shared_write_pos_].r = last_written_r;

        // always advance pointer to keep timeline continuous
        shared_write_pos_ = (shared_write_pos_ + 1) & (kSharedDelaySize - 1);
    }

    output->l = Clip16(out_A * 32768.0f);
    output->r = Clip16(out_B * 32768.0f);
    output++;
  }

   previous_parameters_.raw_algorithm = parameters_.raw_algorithm;
   previous_parameters_.modulation_parameter = parameters_.modulation_parameter;
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
