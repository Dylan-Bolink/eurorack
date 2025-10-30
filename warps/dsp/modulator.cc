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

  de_emphasis_lp_l_.Init();
  de_emphasis_lp_r_.Init();
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

void Modulator::ProcessDelay(ShortFrame* input, ShortFrame* output, size_t size, bool locked_frequency) {
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

    // attenuate output at low sample rate to mask stupid
    // discontinuity bug
    float gain = sample_rate / 0.01f;
    CONSTRAIN(gain, 0.0f, 1.0f);
    wet.l *= gain * gain;
    wet.r *= gain * gain;

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

// void Modulator::ProcessDoppler(ShortFrame* input, ShortFrame* output, size_t size) {
//   ShortFrame *buffer = delay_buffer_;

//   static size_t cursor = 0;
//   static float lfo_phase = 0.0f;
//   static float distance = 1.0f;
//   static float angle = 1.0f;

//   float x = previous_parameters_.raw_algorithm * 2.0f - 1.0f;
//   float x_end = parameters_.raw_algorithm * 2.0f - 1.0f;

//   float y = previous_parameters_.modulation_parameter * 2.0f;
//   float y_end = parameters_.modulation_parameter * 2.0f;

//   float lfo_freq = parameters_.channel_drive[0]
//     * parameters_.channel_drive[0]
//     * 50.0f;
//   float lfo_amplitude = parameters_.channel_drive[1];

//   float step = 1.0f / static_cast<float>(size);
//   float x_increment = (x_end - x) * step;
//   float y_increment = (y_end - y) * step;

//   int8_t shape = parameters_.carrier_shape;

//   float atten_factor =
//     shape == 0 ? 0.5f :
//     shape == 1 ? 4.0f :
//     shape == 2 ? 8.0f :
//     shape == 3 ? 15.0f : 0;

//   float room_size =
//     shape == 0 ? 100 :
//     shape == 1 ? (DELAY_SIZE - 1) / 10.0f :
//     shape == 2 ? (DELAY_SIZE - 1) / 5.0f :
//     shape == 3 ? (DELAY_SIZE - 1) / 2.0f : 0;

//   while (size--) {

//     // write input to buffer
//     buffer[cursor].l = input->l;
//     buffer[cursor].r = input->r;

//     // LFOs
//     float sin = Interpolate(lut_sin, lfo_phase, 1024.0f);
//     float cos = Interpolate(lut_sin + 256, lfo_phase, 1024.0f);

//     float x_lfo = x + sin * lfo_amplitude + 0.05f; // offset avoids discontinuity at 0
//     float y_lfo = y + cos  * lfo_amplitude;
//     CONSTRAIN(x_lfo, -1.0f, 1.0f);
//     CONSTRAIN(y_lfo, -1.0f, 2.0f);

//     // compute angular coordinates
//     float di = sqrtf(x_lfo * x_lfo + y_lfo * y_lfo); // 0..sqrt(5)
//     float an = Interpolate(lut_arcsin, (x_lfo/di + 1.0f) * 0.5f, 256.0f);
//     di /= 2.237;		// sqrt(5)

//     ONE_POLE(distance, di, 0.001f);
//     ONE_POLE(angle, an, 0.001f);

//     // compute binaural delay
//     float binaural_delay = angle * (96000.0f * 0.0015f); // -1.5ms..1.5ms
//     float delay_l = distance * room_size + (angle > 0 ? binaural_delay : 0);
//     float delay_r = distance * room_size + (angle < 0 ? -binaural_delay : 0);

//     // linear delay interpolation
//     MAKE_INTEGRAL_FRACTIONAL(delay_l);
//     MAKE_INTEGRAL_FRACTIONAL(delay_r);

//     int16_t index_l = cursor - delay_l_integral;
//     if (index_l < 0) index_l += DELAY_SIZE;
//     int16_t index_r = cursor - delay_r_integral;
//     if (index_r < 0) index_r += DELAY_SIZE;

//     ShortFrame a_l = buffer[index_l];
//     ShortFrame b_l = buffer[index_l == 0 ? DELAY_SIZE - 1 : index_l - 1];
//     ShortFrame a_r = buffer[index_r];
//     ShortFrame b_r = buffer[index_r == 0 ? DELAY_SIZE - 1 : index_r - 1];

//     short s1_l = a_l.l + (b_l.l - a_l.l) * delay_l_fractional;
//     short s2_l = a_l.r + (b_l.r - a_l.r) * delay_l_fractional;
//     short s1_r = a_r.l + (b_r.l - a_r.l) * delay_r_fractional;
//     short s2_r = a_r.r + (b_r.r - a_r.r) * delay_r_fractional;

//     // distance attenuation
//     float atten = 1.0f + atten_factor * distance * distance;
//     s1_l /= atten;
//     s2_l /= atten;
//     s1_r /= atten;
//     s2_r /= atten;

//     float fade_in = Interpolate(lut_xfade_in, (angle + 1.0f) / 2.0f, 256.0f);
//     float fade_out = Interpolate(lut_xfade_out, (angle + 1.0f) / 2.0f, 256.0f);

//     output->l = s2_l * fade_in + s1_l * fade_out;
//     output->r = s1_r * fade_in + s2_r * fade_out;

//     x += x_increment;
//     y += y_increment;
//     lfo_phase += lfo_freq / 96000.0f;
//     if (lfo_phase > 1.0f) lfo_phase--;
//     input++;
//     output++;
//     cursor = (cursor + 1) % DELAY_SIZE;
//   }

//   previous_parameters_ = parameters_;
// }

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

  case FEATURE_MODE_LOCKED_DELAY:
    ProcessDelay(input, output, size, true);
    // ProcessVocoder(input, output, size);
    break;

  case FEATURE_MODE_DELAY:
    ProcessDelay(input, output, size, false);
    break;

  case FEATURE_MODE_META:
    ProcessMeta(input, output, size);
    break;
  }
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
  // --- VCA & OSCILLATOR LOGIC ---
  float* in_1 = buffer_[0];
  float* in_2 = buffer_[1];
  float* aux = buffer_[2];

  if (!parameters_.carrier_shape) {
    fill(&aux[0], &aux[size], 0.0f);
  }
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
  
  ParameterInterpolator p1_interpolator(
      &previous_parameters_.raw_algorithm,
      parameters_.raw_algorithm,
      size);

  ParameterInterpolator timbre_interpolator(
      &previous_parameters_.modulation_parameter,
      parameters_.modulation_parameter,
      size);

  while (size--) {
    float p1 = p1_interpolator.Next();
    float p2_raw = timbre_interpolator.Next();
    float x = p2_raw * 2.0f - 1.0f;
    float p2_skewed = (x * x * x + 1.0f) * 0.5f;
    float timbre = (p2_skewed * 2.0f) - 1.0f;
    
    float x_1 = *in_1++; 
    float x_2 = *in_2++;

    float fade_in = Interpolate(lut_xfade_in, p1, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, p1, 256.0f);
    
    float mix_A = x_1 * fade_out + x_2 * fade_in; // Main Mix
    float mix_B = x_1 * fade_in + x_2 * fade_out; // Aux (Opposite) Mix

    float effect_amount = fabs(timbre);

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

    if (timbre < 0.0f) {
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
  
      // --- Define the switch threshold ---
      const float stage_switch_threshold = 0.3f;

      // --- Temporary variables for the processed signal ---
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

   if (!parameters_.carrier_shape) {
     fill(&aux[0], &aux[size], 0.0f);
   }
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

   ParameterInterpolator p1_interpolator(
       &previous_parameters_.raw_algorithm,
       parameters_.raw_algorithm,
       size);

   ParameterInterpolator timbre_interpolator(
       &previous_parameters_.modulation_parameter,
       parameters_.modulation_parameter,
       size);

   while (size--) {
     float p1 = p1_interpolator.Next();
     float p2_raw = timbre_interpolator.Next();
     float x = p2_raw * 2.0f - 1.0f;
     float p2_skewed = (x * x * x + 1.0f) * 0.5f;
     float timbre = (p2_skewed * 2.0f) - 1.0f;

     float x_1 = *in_1++;
     float x_2 = *in_2++;

     float fade_in = Interpolate(lut_xfade_in, p1, 256.0f);
     float fade_out = Interpolate(lut_xfade_out, p1, 256.0f);

     float mix_A = x_1 * fade_out + x_2 * fade_in; // Main Mix
     float mix_B = x_1 * fade_in + x_2 * fade_out; // Aux (Opposite) Mix

    float effect_amount = fabs(timbre);

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

   if (timbre < 0.0f) {
      // --- CCW: Cassette Emulation ---

      // --- LFOs (Flutter, Wow, Random Drift) ---
      float flutter_freq = 0.5f + effect_amount * 2.0f;
      float wow_freq = 0.05f + effect_amount * 0.15f;

      static float flutter_lfo_phase = 0.0f;
      flutter_lfo_phase += flutter_freq / 96000.0f;
      if (flutter_lfo_phase >= 1.0f) flutter_lfo_phase -= 1.0f;
      float flutter_mod = Interpolate(lut_sin, flutter_lfo_phase, 1024.0f);

      static float wow_lfo_phase = 0.0f;
      wow_lfo_phase += wow_freq / 96000.0f;
      if (wow_lfo_phase >= 1.0f) wow_lfo_phase -= 1.0f;
      float wow_mod = Interpolate(lut_sin, wow_lfo_phase, 1024.0f);

      static float random_drift = 0.0f;
      ONE_POLE(random_drift, (Random::GetFloat() - 0.5f) * 0.1f, 0.0001f);

      // --- Combine modulations ---
      float mod_intensity_scale = effect_amount;
      float total_mod = (flutter_mod * 0.2f + wow_mod * 0.8f + random_drift) * mod_intensity_scale;

      // --- Read from 1-sample delay for Flanger ---
      const float kFixedTapeDelay = 1.0f;
      float delay_offset = total_mod * 3.0f;
      float read_pos_f = shared_write_pos_ - kFixedTapeDelay + delay_offset;

      // Wrap read position
      while (read_pos_f < 0.0f) read_pos_f += kSharedDelaySize;
      read_pos_f = fmodf(read_pos_f, (float)kSharedDelaySize);

      MAKE_INTEGRAL_FRACTIONAL(read_pos_f);

      // --- Read from buffer ---
      float delayed_l, delayed_r;
      { /* Read Block - No Changes */
          int32_t index_integral = read_pos_f_integral;
          float index_fractional = read_pos_f_fractional;
          const float kToFloat = 1.0f / 32768.0f;
          float xm1_l = static_cast<float>(delay_buffer_[((index_integral - 1) + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
          float x0_l  = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
          float x1_l  = static_cast<float>(delay_buffer_[((index_integral + 1) + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
          float x2_l  = static_cast<float>(delay_buffer_[((index_integral + 2) + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
          float xm1_r = static_cast<float>(delay_buffer_[((index_integral - 1) + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
          float x0_r  = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
          float x1_r  = static_cast<float>(delay_buffer_[((index_integral + 1) + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
          float x2_r  = static_cast<float>(delay_buffer_[((index_integral + 2) + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
          FloatFrame c = { (x1_l - xm1_l) * 0.5f, (x1_r - xm1_r) * 0.5f };
          FloatFrame v = { (float)(x0_l - x1_l), (float)(x0_r - x1_r)};
          FloatFrame w = { c.l + v.l, c.r + v.r };
          FloatFrame a = { w.l + v.l + (x2_l - x0_l) * 0.5f, w.r + v.r + (x2_r - x0_r) * 0.5f };
          FloatFrame b_neg = { w.l + a.l, w.r + a.r };
          float t = index_fractional;
          delayed_l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0_l);
          delayed_r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0_r);
      } /* End Read Block */

      // --- Saturation ---
      float drive = 1.0f + effect_amount * 1.5f; // Increased drive
      float offset = effect_amount * 0.1f;
      float sat_l = stmlib::SoftLimit((delayed_l * drive) + offset);
      float sat_r = stmlib::SoftLimit((delayed_r * drive) + offset);

      // --- De-emphasis ---
      de_emphasis_lp_l_.set_f<stmlib::FREQUENCY_FAST>(0.4f);
      de_emphasis_lp_r_.set_f<stmlib::FREQUENCY_FAST>(0.4f);
      sat_l = de_emphasis_lp_l_.Process<stmlib::FILTER_MODE_LOW_PASS>(sat_l);
      sat_r = de_emphasis_lp_r_.Process<stmlib::FILTER_MODE_LOW_PASS>(sat_r);

      // --- Filtering (Tape Bandwidth) ---
      float filter_cutoff = 0.05f + (1.0f - effect_amount) * 0.45f; // Starts bright, gets dark
      tape_lp_l_.set_f<stmlib::FREQUENCY_FAST>(filter_cutoff);
      tape_lp_r_.set_f<stmlib::FREQUENCY_FAST>(filter_cutoff);
      float filtered_l = tape_lp_l_.Process<stmlib::FILTER_MODE_LOW_PASS>(sat_l);
      float filtered_r = tape_lp_r_.Process<stmlib::FILTER_MODE_LOW_PASS>(sat_r);

      // --- Hiss ---
      const float hiss_threshold = 0.65f;
      float hiss_amount = (effect_amount > hiss_threshold) ? hiss_threshold : effect_amount;
      float hiss = (Random::GetFloat() - 0.5f) * hiss_amount * 0.15f;
      filter_[3].set_f<stmlib::FREQUENCY_FAST>(0.25f);
      float hiss_filtered = filter_[3].Process<stmlib::FILTER_MODE_HIGH_PASS>(hiss);

      processed_A = filtered_l + hiss_filtered;
      processed_B = filtered_r + hiss_filtered;
     } else {
        // --- CW: VHS Emulation ---
        // --- State Variables ---
        static float dropout_check_timer = 0.1f;  // Time until next *check*
        static float dropout_duration_timer = 0.0f; // Time *remaining* in current dropout

        // --- Noise (Band-passed) ---
        float noise = (Random::GetFloat() - 0.5f);
        tape_lp_l_.set_f<stmlib::FREQUENCY_FAST>(0.05f); 
        tape_lp_r_.set_f<stmlib::FREQUENCY_FAST>(0.25f); 
        noise = tape_lp_l_.Process<stmlib::FILTER_MODE_HIGH_PASS>(noise);
        noise = tape_lp_r_.Process<stmlib::FILTER_MODE_LOW_PASS>(noise);

        const float noise_threshold = 0.6f;
        float noise_amount = (effect_amount > noise_threshold) ? noise_threshold : effect_amount;

        // --- Dropout Logic (Improved State Machine) ---
        float dropout_gain = 1.0f;
        float dropout_tilt_mod = 0.0f; // 0 = no dropout, 1 = force dark tilt

        if (dropout_duration_timer > 0.0f) {
            // --- We ARE currently dropping out ---
            dropout_duration_timer -= 1.0f / 96000.0f;
            dropout_gain = 0.2f;   // Don't go to full silence, just "low"
            dropout_tilt_mod = 1.0f; // Force tilt to dark
        } else {
            // --- We are NOT dropping out, check if we should ---
            dropout_check_timer -= 1.0f / 96000.0f;
            if (dropout_check_timer <= 0.0f) {
                float dropout_chance = effect_amount * 0.15f;
                if (Random::GetFloat() < dropout_chance) {
                    // Start a dropout
                    dropout_duration_timer = 0.001f + Random::GetFloat() * 0.015f; // 1-16ms
                }
                // Reset check timer (check more often at high effect)
                dropout_check_timer = 0.01f + Random::GetFloat() * (0.05f * (1.0f - effect_amount));
            }
        }

        // Apply dropout gain *before* filtering
        float signal_A = mix_A * dropout_gain;
        float signal_B = mix_B * dropout_gain;

        // --- Tilt Filter ---
        // Uses 4 filters (0,1,2,3) in PARALLEL
        const float tilt_center_freq = 0.02f; // ~1kHz pivot
        filter_[0].set_f<stmlib::FREQUENCY_FAST>(tilt_center_freq); // L-LPF
        filter_[1].set_f<stmlib::FREQUENCY_FAST>(tilt_center_freq); // L-HPF
        filter_[2].set_f<stmlib::FREQUENCY_FAST>(tilt_center_freq); // R-LPF
        filter_[3].set_f<stmlib::FREQUENCY_FAST>(tilt_center_freq); // R-HPF

        // Process LPF and HPF versions in parallel from the *original* signal
        float tilt_lpf_A = filter_[0].Process<stmlib::FILTER_MODE_LOW_PASS>(signal_A);
        float tilt_hpf_A = filter_[1].Process<stmlib::FILTER_MODE_HIGH_PASS>(signal_A);
        float tilt_lpf_B = filter_[2].Process<stmlib::FILTER_MODE_LOW_PASS>(signal_B);
        float tilt_hpf_B = filter_[3].Process<stmlib::FILTER_MODE_HIGH_PASS>(signal_B);

        // effect_amount = 0.0 -> tilt_mix = 0.5 (flat)
        // effect_amount = 1.0 -> tilt_mix = 1.0 (full dark tilt)
        float tilt_mix = 0.5f + (effect_amount * 0.5f);

        // --- Dropout can override the tilt ---
        if (dropout_tilt_mod > 0.5f) {
            tilt_mix = 1.0f; // Force to 100% LPF (dark)
        }

        float filtered_A = tilt_hpf_A * (1.0f - tilt_mix) + tilt_lpf_A * tilt_mix;
        float filtered_B = tilt_hpf_B * (1.0f - tilt_mix) + tilt_lpf_B * tilt_mix;

        // --- Add Noise (after filtering, so it's bright) ---
        processed_A = filtered_A + (noise * noise_amount * 0.04f);
        processed_B = filtered_B + (noise * noise_amount * 0.04f);
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

    if (!parameters_.carrier_shape) {
      fill(&aux[0], &aux[size], 0.0f);
    }
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

  ParameterInterpolator p1_interpolator(
       &previous_parameters_.raw_algorithm,
       parameters_.raw_algorithm,
       size);

  ParameterInterpolator timbre_interpolator(
       &previous_parameters_.modulation_parameter,
       parameters_.modulation_parameter,
       size);


  while (size--) {
    float p1 = p1_interpolator.Next();
    float p2_raw = timbre_interpolator.Next();
    float x = p2_raw * 2.0f - 1.0f;
    float p2_skewed = (x * x * x + 1.0f) * 0.5f;
    float timbre = (p2_skewed * 2.0f) - 1.0f;

    float x_1 = *in_1++;
    float x_2 = *in_2++;

    float fade_in = Interpolate(lut_xfade_in, p1, 256.0f);
    float fade_out = Interpolate(lut_xfade_out, p1, 256.0f);

    float mix_A = x_1 * fade_out + x_2 * fade_in; // Main Mix
    float mix_B = x_1 * fade_in + x_2 * fade_out; // Aux (Opposite) Mix

    float effect_amount = fabs(timbre);

    // Calculate Fast Crossfade based on absolute amount
    const float xfade_threshold = 0.02f;
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

    if (timbre < 0.0f) {
      //  CCW: Packet Loss / Glitches
      
      // --- Static variables for ALL glitch types ---
      static float glitch_timer = 0.0f;
      static bool currently_glitching = false;
      static float glitch_hold_duration = 0.0f;
      static bool pitch_glitching = false;
      static float glitch_read_pos_f = 0.0f;
      static float glitch_read_speed = 1.0f;
      static bool micro_looping = false;
      static float loop_read_pos_f = 0.0f;
      static float loop_start_pos_f = 0.0f;
      static float loop_length_f = 0.0f;

      const float silence_threshold = 0.0001f;
      bool is_silent = (fabs(mix_A) < silence_threshold) && (fabs(mix_B) < silence_threshold);

      glitch_timer -= 1.0f / 96000.0f;

      if (currently_glitching) {
        if (is_silent) {
            currently_glitching = false;
            pitch_glitching = false;
            micro_looping = false;
        }

        float delayed_l = 0.0f, delayed_r = 0.0f; // Initialized to silence

        if (pitch_glitching) {
            // --- PITCH GLITCH: Read from buffer at new speed ---
            glitch_read_pos_f += glitch_read_speed;
            while (glitch_read_pos_f < 0.0f) {
              glitch_read_pos_f += kSharedDelaySize;
            }
            glitch_read_pos_f = fmodf(glitch_read_pos_f, (float)kSharedDelaySize);

            // Interpolate using your Hermite logic
            MAKE_INTEGRAL_FRACTIONAL(glitch_read_pos_f);
            {
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
                delayed_l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0_l);
                delayed_r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0_r);
            }

        } else if (micro_looping) {
            // --- MICRO-LOOP (Stutter) GLITCH ---
            loop_read_pos_f += 1.0f; // Read at normal speed
            if (loop_read_pos_f >= (loop_start_pos_f + loop_length_f)) {
                loop_read_pos_f = loop_start_pos_f; // Wrap back to start
            }
            loop_read_pos_f = fmodf(loop_read_pos_f, (float)kSharedDelaySize);

            // Interpolate using your Hermite logic (Again)
            MAKE_INTEGRAL_FRACTIONAL(loop_read_pos_f);
            {
                int32_t index_integral = loop_read_pos_f_integral;
                float index_fractional = loop_read_pos_f_fractional;
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
                delayed_l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0_l);
                delayed_r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0_r);
            }
        }
        
        processed_A = delayed_l;
        processed_B = delayed_r;

        // --- Check for end of glitch (applies to both types) ---
        glitch_hold_duration -= 1.0f / 96000.0f;
        if (glitch_hold_duration <= 0.0f) {
            currently_glitching = false;
            pitch_glitching = false;
            micro_looping = false;
            glitch_timer = 0.005f + Random::GetFloat() * 0.01f;
        }

      } else {
        // --- Normal Pass-through ---
        processed_A = mix_A;
        processed_B = mix_B;

        // --- Check if we should start a glitch ---
        if (glitch_timer <= 0.0f) {
          float glitch_chance = effect_amount * 0.5f; // Using your faster 0.5f chance
          
          // --- Don't start a new glitch if silent ---
          if (Random::GetFloat() < glitch_chance && !is_silent) {
            
            // --- START GLITCH ---
            currently_glitching = true;
            glitch_hold_duration = 0.001f + Random::GetFloat() * (0.25f * effect_amount); 
            
            if (Random::GetFloat() < effect_amount) {
                // --- YES, PITCH GLITCH ---
                pitch_glitching = true;
                micro_looping = false;
                
                float random_mod = Random::GetFloat() * 0.1f - 0.05f;
                int speed_choice = static_cast<int>(Random::GetFloat() * 3.0f);
                if (speed_choice == 0)      glitch_read_speed = 0.5f + random_mod;
                else if (speed_choice == 1) glitch_read_speed = 2.0f + random_mod;
                else                        glitch_read_speed = -1.0f + random_mod;
                
                int32_t random_offset = 100 + static_cast<int>(Random::GetFloat() * (kSharedDelaySize - 200));
                glitch_read_pos_f = (float)((shared_write_pos_ - random_offset + kSharedDelaySize) % kSharedDelaySize);

            } else {
                // --- NO, do a MICRO-LOOP GLITCH ---
                pitch_glitching = false;
                micro_looping = true;
                
                loop_length_f = 480.0f + Random::GetFloat() * 1920.0f; // 5-25ms
                int32_t random_offset = 100 + static_cast<int>(Random::GetFloat() * 4700.0f); // last 50ms
                loop_start_pos_f = (float)((shared_write_pos_ - random_offset + kSharedDelaySize) % kSharedDelaySize);
                loop_read_pos_f = loop_start_pos_f;
            }

          } else {
            // No glitch this time, reset check timer
            glitch_timer = 0.01f + Random::GetFloat() * (0.05f * (1.0f - effect_amount));
          }
        }
      }
    } else {
        // --- CW: Granular Smear / Stasis  ---
        // --- State ---
        static float current_delay_samples_ = 2880.0f;
        static float feedback_l = 0.0f, feedback_r = 0.0f;
        static float smear_lfo_phase_ = 0.0f;
        static float smear_lfo_target_val_ = 0.5f;
        static float smear_lfo_current_val_ = 0.5f;
        static float smoothed_input_mix = 0.0f;

        // Calculate Target Delay
        float base_delay_samples = 2880.0f + effect_amount * 8640.0f;
        
        // Slew the current delay time
        ONE_POLE(current_delay_samples_, base_delay_samples, 0.001f);

        // --- STASIS LOGIC  ---
        float delta = fabs(base_delay_samples - current_delay_samples_);
        const float stasis_threshold = 0.1f;
        float input_mix = (delta > stasis_threshold) ? 1.0f : 0.0f; // Raw 0/1 signal
        
        // --- Smooth the raw mix, but with different attack/release ---
        // Attack is fast (0.005f), Release is slow (0.0001f)
        float slew_rate = (input_mix > smoothed_input_mix) ? 0.005f : 0.0001f;
        ONE_POLE(smoothed_input_mix, input_mix, slew_rate);
        
        // --- Update the Smear LFO ---
        smear_lfo_phase_ += 0.5f / 96000.0f;
        if (smear_lfo_phase_ >= 1.0f) {
            smear_lfo_phase_ -= 1.0f;
            smear_lfo_target_val_ = Random::GetFloat();
        }
        ONE_POLE(smear_lfo_current_val_, smear_lfo_target_val_, 0.0005f);
        float smear_mod = (smear_lfo_current_val_ - 0.5f) * 2.0f;

        // --- Calculate Final Read Positions ---
        // Increased LFO modulation from 960.0f to 2880.0f (3x)
        float smear_mod_amount = effect_amount * 2880.0f * smoothed_input_mix;
        float stereo_offset = effect_amount * 200.0f * smoothed_input_mix;
        
        float delay_l_samples = current_delay_samples_ - stereo_offset + (smear_mod * smear_mod_amount);
        float delay_r_samples = current_delay_samples_ + stereo_offset - (smear_mod * smear_mod_amount); // Invert LFO for R

        float read_pos_l_to_use = (float)shared_write_pos_ - delay_l_samples;
        float read_pos_r_to_use = (float)shared_write_pos_ - delay_r_samples;
        
        // --- Read audio (L Channel) ---
        float delayed_l;
        { /* (Identical Read Block) */ 
            float read_pos = read_pos_l_to_use;
            while (read_pos < 0.0f) read_pos += kSharedDelaySize;
            read_pos = fmodf(read_pos, (float)kSharedDelaySize);
            MAKE_INTEGRAL_FRACTIONAL(read_pos);
            int32_t index_integral = read_pos_integral;
            float index_fractional = read_pos_fractional;
            const float kToFloat = 1.0f / 32768.0f;
            float xm1_l = static_cast<float>(delay_buffer_[((index_integral - 1) + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
            float x0_l  = static_cast<float>(delay_buffer_[index_integral].l) * kToFloat;
            float x1_l  = static_cast<float>(delay_buffer_[((index_integral + 1) + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
            float x2_l  = static_cast<float>(delay_buffer_[((index_integral + 2) + kSharedDelaySize) % kSharedDelaySize].l) * kToFloat;
            FloatFrame c = { (x1_l - xm1_l) * 0.5f, 0.0f };
            FloatFrame v = { (float)(x0_l - x1_l), 0.0f};
            FloatFrame w = { c.l + v.l, 0.0f };
            FloatFrame a = { w.l + v.l + (x2_l - x0_l) * 0.5f, 0.0f };
            FloatFrame b_neg = { w.l + a.l, 0.0f };
            float t = index_fractional;
            delayed_l = ((((a.l * t) - b_neg.l) * t + c.l) * t + x0_l);
        }

        // --- Read audio (R Channel) ---
        float delayed_r;
        { /* (Identical Read Block) */
            float read_pos = read_pos_r_to_use;
            while (read_pos < 0.0f) read_pos += kSharedDelaySize;
            read_pos = fmodf(read_pos, (float)kSharedDelaySize);
            MAKE_INTEGRAL_FRACTIONAL(read_pos);
            int32_t index_integral = read_pos_integral;
            float index_fractional = read_pos_fractional;
            const float kToFloat = 1.0f / 32768.0f;
            float xm1_r = static_cast<float>(delay_buffer_[((index_integral - 1) + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
            float x0_r  = static_cast<float>(delay_buffer_[index_integral].r) * kToFloat;
            float x1_r  = static_cast<float>(delay_buffer_[((index_integral + 1) + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
            float x2_r  = static_cast<float>(delay_buffer_[((index_integral + 2) + kSharedDelaySize) % kSharedDelaySize].r) * kToFloat;
            FloatFrame c = { 0.0f, (x1_r - xm1_r) * 0.5f };
            FloatFrame v = { 0.0f, (float)(x0_r - x1_r)};
            FloatFrame w = { 0.0f, c.r + v.r };
            FloatFrame a = { 0.0f, w.r + v.r + (x2_r - x0_r) * 0.5f };
            FloatFrame b_neg = { 0.0f, w.r + a.r };
            float t = index_fractional;
            delayed_r = ((((a.r * t) - b_neg.r) * t + c.r) * t + x0_r);
        }

        // --- Feedback (for smearing) ---
        float feedback_amount = effect_amount * 0.92f * smoothed_input_mix; 

        float current_out_l = delayed_l;
        float current_out_r = delayed_r;
        delayed_l += feedback_l * feedback_amount;
        delayed_r += feedback_r * feedback_amount;
        
        feedback_l = current_out_l * smoothed_input_mix; 
        feedback_r = current_out_r * smoothed_input_mix;
        
        delayed_l = stmlib::SoftLimit(delayed_l);
        delayed_r = stmlib::SoftLimit(delayed_r);

        // --- Set Output ---
        processed_A = delayed_l;
        processed_B = delayed_r;
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
