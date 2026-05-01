// Copyright 2015 Emilie Gillet.
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
// Generator for the X/Y outputs.

#include "marbles/random/x_y_generator.h"

#include <algorithm>

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/units.h"

#include "marbles/random/distributions.h"
#include "marbles/resources.h"

namespace marbles {

using namespace std;
using namespace stmlib;


void XYGenerator::Init(RandomStream* random_stream, float sr) {
  for (size_t i = 0; i < kNumChannels; ++i) {
    random_sequence_[i].Init(random_stream);
    output_channel_[i].Init();
  }
  ramp_extractor_.Init(8000.0f / sr);
  ramp_divider_.Init();
  external_clock_stabilization_counter_ = 16;
  
  fill(
      &use_shifted_sequences_[0],
      &use_shifted_sequences_[kNumChannels],
      false);
  rr_counter_ = 0;
  rr_prev_phase_ = 0.0f;
  fill(&rr_held_[0], &rr_held_[kNumXChannels], 0.0f);
  fill(&env_phase_[0], &env_phase_[kNumXChannels], 1.0f);
  fill(&env_rate_[0], &env_rate_[kNumXChannels], 0.0f);
  fill(&env_prev_ramp_[0], &env_prev_ramp_[kNumXChannels], 0.0f);
}

const uint32_t hashes[kNumXChannels] = {
  0, 0xbeca55e5, 0xf0cacc1a
};

void XYGenerator::Process(
    ClockSource clock_source,
    const GroupSettings& x_settings,
    const GroupSettings& y_settings,
    bool* reset,
    const GateFlags* external_clock,
    const Ramps& ramps,
    float* output,
    size_t size) {
  float* channel_ramp[kNumChannels];
  
  if (clock_source != CLOCK_SOURCE_EXTERNAL) {
    // For a couple of upcoming blocks, we'll still be receiving garbage from
    // the normalization pin that we need to ignore.
    external_clock_stabilization_counter_ = 16;
  } else {
    if (external_clock_stabilization_counter_) {
      --external_clock_stabilization_counter_;
      if (external_clock_stabilization_counter_ == 0) {
        ramp_extractor_.Reset();
      }
    }
  }
  
  switch (clock_source) {
    case CLOCK_SOURCE_EXTERNAL:
      {
        Ratio r = { 1, 1 };
        ramp_extractor_.Process(
            r, false, reset, external_clock, ramps.slave[0], size);
        if (external_clock_stabilization_counter_) {
          fill(&ramps.slave[0][0], &ramps.slave[0][size], 0.0f);
        }
      }
      channel_ramp[0] = ramps.slave[0];
      channel_ramp[1] = ramps.slave[0];
      channel_ramp[2] = ramps.slave[0];
      break;
      
    case CLOCK_SOURCE_INTERNAL_T1:
      channel_ramp[0] = ramps.slave[0];
      channel_ramp[1] = ramps.slave[0];
      channel_ramp[2] = ramps.slave[0];
      break;

    case CLOCK_SOURCE_INTERNAL_T2:
      channel_ramp[0] = ramps.master;
      channel_ramp[1] = ramps.master;
      channel_ramp[2] = ramps.master;
      break;
      
    case CLOCK_SOURCE_INTERNAL_T3:
      channel_ramp[0] = ramps.slave[1];
      channel_ramp[1] = ramps.slave[1];
      channel_ramp[2] = ramps.slave[1];
      break;
      
    default:
      channel_ramp[0] = ramps.slave[0];
      channel_ramp[1] = ramps.master;
      channel_ramp[2] = ramps.slave[1];
      break;
  }
  
  if (x_settings.control_mode == CONTROL_MODE_ROUND_ROBIN) {
    float* rr_clock = (clock_source == CLOCK_SOURCE_EXTERNAL)
                      ? ramps.slave[0] : ramps.master;
    channel_ramp[0] = channel_ramp[1] = channel_ramp[2] = rr_clock;
  }

  if (x_settings.control_mode == CONTROL_MODE_ENVELOPE
      && clock_source != CLOCK_SOURCE_INTERNAL_T1_T2_T3) {
    float* env_clock = (clock_source == CLOCK_SOURCE_EXTERNAL)
                        ? ramps.slave[0] : channel_ramp[0];
    channel_ramp[0] = channel_ramp[1] = channel_ramp[2] = env_clock;
  }

  if (*reset) {
    ramp_divider_.Reset();
    rr_counter_ = 0;
    rr_prev_phase_ = 0.0f;
    fill(&env_phase_[0], &env_phase_[kNumXChannels], 1.0f);
    fill(&env_prev_ramp_[0], &env_prev_ramp_[kNumXChannels], 0.0f);
  }
  
  ramp_divider_.Process(y_settings.ratio, channel_ramp[1], ramps.external, size);
  channel_ramp[kNumChannels - 1] = ramps.external;
  
  for (size_t i = 0; i < kNumChannels; ++i) {
    OutputChannel& channel = output_channel_[i];
    const GroupSettings& settings = i < kNumXChannels ? x_settings : y_settings;
    
    switch (settings.voltage_range) {
      case VOLTAGE_RANGE_NARROW:
        channel.set_scale_offset(ScaleOffset(2.0f, 0.0f));
        break;
      
      case VOLTAGE_RANGE_POSITIVE:
        channel.set_scale_offset(ScaleOffset(5.0f, 0.0f));
        break;
      
      case VOLTAGE_RANGE_FULL:
        channel.set_scale_offset(ScaleOffset(10.0f, -5.0f));
        break;
      
      default:
        break;
    }
    
    float amount = 1.0f;
    if (settings.control_mode == CONTROL_MODE_BUMP) {
      amount = i == kNumXChannels / 2 ? 1.0f : -1.0f;
    } else if (settings.control_mode == CONTROL_MODE_TILT) {
      amount = 2.0f * static_cast<float>(i) / float(kNumXChannels - 1) - 1.0f;
    }
    
    channel.set_spread(0.5f + (settings.spread - 0.5f) * amount);
    channel.set_bias(0.5f + (settings.bias - 0.5f) * amount);
    channel.set_steps(0.5f + (settings.steps - 0.5f) * \
        (settings.register_mode ? 1.0f : amount));
    channel.set_scale_index(settings.scale_index);
    channel.set_register_mode(settings.register_mode);
    channel.set_register_value(settings.register_value);
    channel.set_register_transposition(
        4.0f * settings.spread * (settings.bias - 0.5f) * amount);
    
    RandomSequence* sequence = &random_sequence_[i];
    sequence->Record();
    sequence->set_length(settings.length);
    sequence->set_deja_vu(settings.deja_vu);
    if (*reset) {
      sequence->Reset();
    }
    
    bool use_shifted_sequences = false;
    
    // When all channels follow the same clock, the deja-vu random looping will
    // follow the same pattern and the constant-mode input will be shifted!
    if (clock_source != CLOCK_SOURCE_INTERNAL_T1_T2_T3
        && i > 0 && i < kNumXChannels
        && x_settings.control_mode != CONTROL_MODE_ENVELOPE) {
      sequence = &random_sequence_[0];
      if (settings.use_shift_register) {
        use_shifted_sequences = true;

        if (settings.control_mode == CONTROL_MODE_IDENTICAL
            || settings.control_mode == CONTROL_MODE_ROUND_ROBIN) {
          sequence->ReplayShifted(i);
        } else if (settings.control_mode == CONTROL_MODE_BUMP) {
          sequence->ReplayShifted(i == 2 ? 1 : 0);
        } else {
          sequence->ReplayShifted(0);
        }
      } else {
        sequence->ReplayPseudoRandom(hashes[i]);
      }
    }
    
    if (!use_shifted_sequences && use_shifted_sequences_[i]) {
      sequence->Clone(random_sequence_[0]);
    }
    use_shifted_sequences_[i] = use_shifted_sequences;
    
    if (x_settings.control_mode == CONTROL_MODE_ENVELOPE && i < kNumXChannels) {
      // Envelope mode: skip
    } else if (x_settings.control_mode == CONTROL_MODE_ROUND_ROBIN
        && i < kNumXChannels
        && i != static_cast<size_t>(rr_counter_)) {
      for (size_t s = 0; s < size; s++) {
        output[i + s * kNumChannels] = rr_held_[i];
      }
    } else {
      channel.Process(sequence, channel_ramp[i], &output[i], size, kNumChannels);
    }
  }

  if (x_settings.control_mode == CONTROL_MODE_ENVELOPE) {
    const float bias = x_settings.bias;
    const float spread = x_settings.spread;
    const float steps = x_settings.steps;

    bool external_clock = (clock_source != CLOCK_SOURCE_INTERNAL_T1_T2_T3);
    const float output_scale = 5.0f;

    for (size_t s = 0; s < size; s++) {
      if (external_clock) {
        float prev = (s == 0) ? rr_prev_phase_ : channel_ramp[0][s - 1];
        if (channel_ramp[0][s] < prev) {
          rr_counter_ = (rr_counter_ + 1) % static_cast<int>(kNumXChannels);
        }
      }

      for (size_t ch = 0; ch < kNumXChannels; ch++) {
        float* ramp = channel_ramp[ch];

        // Trigger detection
        bool triggered = false;
        if (external_clock) {
          float prev = (s == 0) ? rr_prev_phase_ : ramp[s - 1];
          triggered = (ramp[s] < prev)
              && (ch == static_cast<size_t>(rr_counter_));
        } else {
          float prev = (s == 0) ? env_prev_ramp_[ch] : ramp[s - 1];
          triggered = (ramp[s] < prev);
        }

        if (triggered) {
          float phase = env_phase_[ch];
          float af = steps;
          CONSTRAIN(af, 0.001f, 0.999f);

          // NARROW = hard reset, POSITIVE = serge, FULL = legato
          if (x_settings.voltage_range == VOLTAGE_RANGE_POSITIVE) {
            if (phase < af) triggered = false;
          } else if (x_settings.voltage_range == VOLTAGE_RANGE_FULL) {
            if (phase < 1.0f) triggered = false;
          }
        }

        if (triggered) {
          float u = random_sequence_[ch].NextValue(false, 0.0f);

          // Same distribution as normal X mode
          float degenerate_amount = 1.25f - spread * 25.0f;
          float bernoulli_amount = spread * 25.0f - 23.75f;
          CONSTRAIN(degenerate_amount, 0.0f, 1.0f);
          CONSTRAIN(bernoulli_amount, 0.0f, 1.0f);

          float time_value = BetaDistributionSample(u, spread, bias);
          float bernoulli_value = u >= (1.0f - bias) ? 0.999999f : 0.0f;
          time_value += degenerate_amount * (bias - time_value);
          time_value += bernoulli_amount * (bernoulli_value - time_value);

          // Skip fastest 20% of time range
          time_value = 0.2f + time_value * 0.8f;

          env_rate_[ch] = (1.0f / 32.0f)
              * SemitonesToRatioSafe(-156.0f * time_value);
          env_phase_[ch] = 0.0f;
        }

        // Compute envelope shape using phase-warped raised cosine
        float value = 0.0f;
        float phase = env_phase_[ch];

        if (phase < 1.0f) {
          float attack_fraction = steps;
          CONSTRAIN(attack_fraction, 0.001f, 0.999f);

          if (phase < attack_fraction) {
            // Attack phase
            float t = phase / attack_fraction;
            float swell = steps * steps;
            float t_cubed = t * t * t;
            float t_warped = t + swell * (t_cubed - t);
            value = Interpolate(lut_raised_cosine, t_warped, 256.0f);
          } else {
            // Decay phase
            float t = (phase - attack_fraction) / (1.0f - attack_fraction);
            float snap = (1.0f - steps) * (1.0f - steps);
            float inv = 1.0f - t;
            float t_fast = 1.0f - inv * inv * inv;
            float t_warped = t + snap * (t_fast - t);
            value = 1.0f - Interpolate(lut_raised_cosine, t_warped, 256.0f);
          }
        } else {
          value = (steps > 0.99f) ? 1.0f : 0.0f;
        }

        CONSTRAIN(value, 0.0f, 1.0f);
        output[ch + s * kNumChannels] = value * output_scale;
        env_phase_[ch] += env_rate_[ch];
      }
    }

    // Update previous ramp trackers
    if (external_clock) {
      rr_prev_phase_ = channel_ramp[0][size - 1];
    }
    for (size_t ch = 0; ch < kNumXChannels; ch++) {
      env_prev_ramp_[ch] = channel_ramp[ch][size - 1];
    }
  }

  if (x_settings.control_mode == CONTROL_MODE_ROUND_ROBIN) {
    const float* base_ramp = channel_ramp[0];
    int active_channel = rr_counter_;
    for (size_t s = 0; s < size; s++) {
      float prev = (s == 0) ? rr_prev_phase_ : base_ramp[s - 1];
      if (base_ramp[s] < prev) {
        rr_held_[active_channel] = output[s * kNumChannels + active_channel];
        rr_counter_ = (rr_counter_ + 1) % static_cast<int>(kNumXChannels);
       
        output_channel_[rr_counter_].set_previous_phase(base_ramp[s]);
        break;
      }
    }
    rr_prev_phase_ = base_ramp[size - 1];
    for (size_t s = 0; s < size; s++) {
      for (int ch = 0; ch < static_cast<int>(kNumXChannels); ch++) {
        if (ch != active_channel) {
          output[s * kNumChannels + ch] = rr_held_[ch];
        }
      }
    }
  }
}

}  // namespace marbles