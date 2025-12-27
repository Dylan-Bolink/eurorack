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
// Generator for the T outputs.

#include "marbles/random/t_generator.h"
#include <algorithm>
#include "stmlib/dsp/units.h"
#include "marbles/resources.h"

namespace marbles {
  
using namespace std;
using namespace stmlib;

/* static */
DividerPattern TGenerator::divider_patterns[kNumDividerPatterns] = {
  { { { 1, 1 }, { 1, 1 } }, 1 },
  { { { 1, 1 }, { 2, 1 } }, 1 },
  { { { 1, 2 }, { 1, 1 } }, 2 },
  { { { 1, 1 }, { 4, 1 } }, 1 },
  { { { 1, 2 }, { 2, 1 } }, 2 },
  { { { 1, 1 }, { 3, 2 } }, 2 },
  { { { 1, 4 }, { 4, 1 } }, 4 },
  { { { 1, 4 }, { 2, 1 } }, 4 },
  { { { 1, 2 }, { 3, 2 } }, 2 },
  { { { 1, 1 }, { 8, 1 } }, 1 },
  { { { 1, 1 }, { 3, 1 } }, 1 },
  { { { 1, 3 }, { 1, 1 } }, 3 },
  { { { 1, 1 }, { 5, 4 } }, 4 },
  { { { 1, 2 }, { 5, 4 } }, 4 },
  { { { 1, 1 }, { 6, 1 } }, 1 },
  { { { 1, 3 }, { 2, 1 } }, 3 },
  { { { 1, 1 }, { 16, 1 } }, 1 },
};

/* static */
DividerPattern TGenerator::fixed_divider_patterns[kNumDividerPatterns] = {
  { { { 8, 1 }, { 1, 8 } }, 8 },
  { { { 6, 1 }, { 1, 6 } }, 6 },
  { { { 4, 1 }, { 1, 4 } }, 4 },
  { { { 3, 1 }, { 1, 3 } }, 3 },
  { { { 2, 1 }, { 1, 2 } }, 2 },
  { { { 3, 2 }, { 2, 3 } }, 6 },
  { { { 4, 3 }, { 3, 4 } }, 12 },
  { { { 5, 4 }, { 4, 5 } }, 20 },
  { { { 1, 1 }, { 1, 1 } }, 1 },
  { { { 4, 5 }, { 5, 4 } }, 20 },
  { { { 3, 4 }, { 4, 3 } }, 12 },
  { { { 2, 2 }, { 3, 2 } }, 6 },
  { { { 1, 2 }, { 2, 1 } }, 2 },
  { { { 1, 3 }, { 3, 1 } }, 3 },
  { { { 1, 4 }, { 4, 1 } }, 4 },
  { { { 1, 6 }, { 6, 1 } }, 6 },
  { { { 1, 8 }, { 8, 1 } }, 8 },
};

/* static */
uint8_t TGenerator::drum_patterns[kNumDrumPatterns][kDrumPatternSize] = {
  { 1, 0, 0, 0, 2, 0, 0, 0 },
  { 0, 0, 1, 0, 2, 0, 0, 0 },
  { 1, 0, 1, 0, 2, 0, 0, 0 },
  { 0, 0, 1, 0, 2, 0, 0, 2 },
  { 1, 0, 1, 0, 2, 0, 1, 0 },
  { 0, 2, 1, 0, 2, 0, 0, 2 },
  { 1, 0, 0, 0, 2, 0, 1, 0 },
  { 0, 2, 1, 0, 2, 0, 1, 2 },
  { 1, 0, 0, 1, 2, 0, 0, 0 },
  { 0, 2, 1, 1, 2, 0, 1, 2 },
  { 1, 0, 0, 1, 2, 0, 1, 0 },
  { 0, 2, 1, 1, 2, 2, 1, 2 },
  { 1, 0, 0, 1, 2, 0, 1, 2 },
  { 0, 2, 0, 1, 2, 0, 1, 2 },
  { 1, 0, 1, 1, 2, 0, 1, 2 },
  { 2, 0, 1, 2, 0, 1, 2, 0 },
  { 1, 2, 1, 1, 2, 0, 1, 2 },
  { 2, 0, 1, 2, 0, 1, 2, 2 }
};

/* static */
Ratio TGenerator::input_divider_ratios[kNumInputDividerRatios] = {
  { 1, 4 }, { 1, 3 }, { 1, 2 }, { 2, 3 }, { 1, 1 },
  { 3, 2 }, { 2, 1 }, { 3, 1 }, { 4, 1 },
};

void TGenerator::Init(RandomStream* random_stream, float sr) {
  one_hertz_ = 1.0f / static_cast<float>(sr);
  model_ = T_GENERATOR_MODEL_COMPLEMENTARY_BERNOULLI;
  range_ = T_GENERATOR_RANGE_1X;
  rate_ = 0.0f;
  bias_ = 0.5f;
  jitter_ = 0.0f;
  pulse_width_mean_ = 0.0f;
  pulse_width_std_ = 0.0f;
  master_phase_ = 0.0f;
  jitter_multiplier_ = 1.0f;
  phase_difference_ = 0.0f;
  previous_external_ramp_value_ = 0.0f;
  divider_pattern_length_ = 0;
  fill(&streak_counter_[0], &streak_counter_[kMarkovHistorySize], 0);
  fill(&markov_history_[0], &markov_history_[kMarkovHistorySize], 0);
  markov_history_ptr_ = 0;
  drum_pattern_step_ = 0;
  drum_pattern_index_ = 0;
  sequence_.Init(random_stream);
  ramp_extractor_.Init(1000.0f / sr);
  ramp_generator_.Init();
  for (size_t i = 0; i < kNumTChannels; ++i) {
    slave_ramp_[i].Init();
  }
  bias_quantizer_.Init(kNumDividerPatterns, 0.1f, false);
  rate_quantizer_.Init(kNumInputDividerRatios, 0.05f, false);
  use_external_clock_ = false;

  // --- Truchets init ---
  grids_.Init();
  grids_x_ = 0.5f; 
  grids_y_ = 0.5f;
  grids_chaos_ = 0.0f;
  dens_kick_ = 0.5f;
  dens_snare_ = 0.5f;
  dens_hh_ = 0.5f;
  grids_length_ = 32;
  grids_accent_state_ = false;
  grids_hh_state_ = false; 
  
  hh_trigger_ = false;
  prev_hh_state_ = false;
  current_period_ = 0.0f;


  hh_pulse_length_ = 0;
  hh_pulse_counter_ = 0;
  sample_index_ = 0;
  fill(&hh_gate_buffer_[0], &hh_gate_buffer_[kBlockSize], false);
  fill(&accent_gate_buffer_[0], &accent_gate_buffer_[kBlockSize], false);
}

int TGenerator::GenerateComplementaryBernoulli(const RandomVector& x) {
  int bitmask = 0;
  for (size_t i = 0; i < kNumTChannels; ++i) {
    if ((x.variables.u[i >> 1] > bias_) ^ (i & 1)) {
      bitmask |= 1 << i;
    }
  }
  return bitmask;
}

int TGenerator::GenerateIndependentBernoulli(const RandomVector& x) {
  int bitmask = 0;
  for (size_t i = 0; i < kNumTChannels; ++i) {
    if ((x.variables.u[i] > bias_) ^ (i & 1)) {
      bitmask |= 1 << i;
    }
  }
  return bitmask;
}

int TGenerator::GenerateDrums(const RandomVector& x) {
  ++drum_pattern_step_;
  if (drum_pattern_step_ >= kDrumPatternSize) {
    drum_pattern_step_ = 0;
    float u = x.variables.u[0] * 2.0f * fabs(bias_ - 0.5f);
    drum_pattern_index_ = static_cast<int32_t>(kNumDrumPatterns * u);
    if (bias_ <= 0.5f) {
      drum_pattern_index_ -= drum_pattern_index_ % 2;
    }
  }
  return drum_patterns[drum_pattern_index_][drum_pattern_step_];
}

int TGenerator::GenerateMarkov(const RandomVector& x) {
  int bitmask = 0;
  float b = 1.5f * bias_ - 0.5f;
  markov_history_[markov_history_ptr_] = 0;
  const int32_t p = markov_history_ptr_;
  for (size_t i = 0; i < kNumTChannels; ++i) {
    int32_t mask = 1 << i;
    // 4 rules:
    // * We favor repeating what we played 8 ticks ago.
    // * We do not favor pulses appearing on both channels.
    // * We favor sparse patterns (no consecutive hits).
    // * We favor patterns in which one channel "echoes" what the other
    //   channel played 4 ticks before.
    bool periodic = markov_history_[(p + 8) % kMarkovHistorySize] & mask;
    bool simultaneous = markov_history_[(p + 8) % kMarkovHistorySize] & ~mask;
    bool dense = markov_history_[(p + 1) % kMarkovHistorySize] & mask;
    bool alternate = markov_history_[(p + 4) % kMarkovHistorySize] & ~mask;

    float logit = -1.5f;
    logit += streak_counter_[i] > 24 ? 10.0f : 0.0f;
    logit += 8.0f * fabs(b) * (periodic ? b : -b);
    logit -= 2.0f * (simultaneous ? b : -b);
    logit -= 1.0f * (dense ? b : 0.0f);
    logit += 1.0f * (alternate ? b : 0.0f);
    CONSTRAIN(logit, -10.0f, 10.0f);
    float probability = lut_logit[static_cast<int>(logit * 12.8f + 128.0f)];
    bool state = x.variables.u[i] < probability;
    
    if (sequence_.deja_vu() >= x.variables.p) {
      state = markov_history_[(p + sequence_.length()) % kMarkovHistorySize] & mask;
    }
    if (state) {
      bitmask |= mask;
      streak_counter_[i] = 0;
    } else {
      ++streak_counter_[i];
    }
  }
  markov_history_[p] |= bitmask;
  markov_history_ptr_ = (p + kMarkovHistorySize - 1) % kMarkovHistorySize;
  return bitmask;
}

void TGenerator::ScheduleOutputPulses(const RandomVector& x, int bitmask) {
  for (size_t i = 0; i < kNumTChannels; ++i) {
    slave_ramp_[i].Init(
        bitmask & 1,
        RandomPulseWidth(i, x.variables.pulse_width[i]),
        0.5f);
    bitmask >>= 1;
  }
}

int TGenerator::GenerateGrids(const RandomVector& x) {
  ++drum_pattern_step_;
  size_t len = static_cast<size_t>(grids_length_);
  if (len < 1) len = 32;
  if (drum_pattern_step_ >= len) drum_pattern_step_ = 0;

  uint8_t read_step = drum_pattern_step_ % 32;
  uint8_t x_u8 = static_cast<uint8_t>(grids_x_ * 255.0f);
  uint8_t y_u8 = static_cast<uint8_t>(grids_y_ * 255.0f);

  // idea for later? alternate coordinate quantization based on setting
  // if (range_ == T_GENERATOR_RANGE_4X) { // RED (MF Linear)
  //     uint8_t p_idx = static_cast<uint8_t>(grids_x_ * 24.9f);
  //     x_u8 = (p_idx % 5) * 64; y_u8 = (p_idx / 5) * 64;
  //     read_step = (read_step + static_cast<uint8_t>(grids_y_ * 15.0f)) % 32;
  // } else if (range_ == T_GENERATOR_RANGE_1X) { // ORANGE (Discrete)
  //     x_u8 = static_cast<uint8_t>(grids_x_ * 4.9f) * 64;
  //     y_u8 = static_cast<uint8_t>(grids_y_ * 4.9f) * 64;
  // } else { // GREEN (Smooth)
      // x_u8 = static_cast<uint8_t>(grids_x_ * 255.0f);
      // y_u8 = static_cast<uint8_t>(grids_y_ * 255.0f);
  // }

  grids::PatternGenerator::OutputStep result = grids_.GetStep(read_step, x_u8, y_u8);

  uint8_t chaos_amt = static_cast<uint8_t>(grids_chaos_ * 64.0f);
  
  uint8_t p[3];
  p[0] = static_cast<uint8_t>(x.variables.u[0] * chaos_amt);
  p[1] = static_cast<uint8_t>(x.variables.u[1] * chaos_amt);
  p[2] = static_cast<uint8_t>((x.variables.u[0] + x.variables.u[1]) * 0.5f * chaos_amt);

  int bitmask = 0;
  const float kDead = 0.03f;

  if (dens_kick_ > kDead) {
      uint8_t lvl = result.bd;
      if (lvl < (255 - p[0])) lvl += p[0]; else lvl = 255;
      uint8_t thresh = static_cast<uint8_t>((1.0f - dens_kick_) * 255.0f);
      if (lvl > thresh) bitmask |= 1;
  }

  if (dens_snare_ > kDead) {
      uint8_t lvl = result.sd;
      if (lvl < (255 - p[1])) lvl += p[1]; else lvl = 255;
      
      uint8_t thresh = static_cast<uint8_t>((1.0f - dens_snare_) * 255.0f);
      if (lvl > thresh) bitmask |= (1 << 1);
  }

  if (dens_hh_ > kDead) {
    uint8_t lvl = result.hh;
    if (lvl < (255 - p[2])) lvl += p[2]; else lvl = 255;

    uint8_t thresh = static_cast<uint8_t>((1.0f - dens_hh_) * 255.0f);
    bool trigger = (lvl > thresh);
    
    if (trigger) {
        float pulse_width = 0.05f + 0.9f * pulse_width_mean_;
        hh_pulse_length_ = static_cast<int>(current_period_ * pulse_width);
        if (hh_pulse_length_ < 1) hh_pulse_length_ = 1;
        hh_pulse_counter_ = hh_pulse_length_;
    }
    
    grids_hh_state_ = trigger;
  } else {
      grids_hh_state_ = false;
  }

  grids_accent_state_ = (result.accent > 0);
  return bitmask;
}

void TGenerator::ConfigureSlaveRamps(const RandomVector& x) {
  switch (model_) {
    case T_GENERATOR_MODEL_COMPLEMENTARY_BERNOULLI: ScheduleOutputPulses(x, GenerateComplementaryBernoulli(x)); break;
    case T_GENERATOR_MODEL_INDEPENDENT_BERNOULLI: ScheduleOutputPulses(x, GenerateIndependentBernoulli(x)); break;
    case T_GENERATOR_MODEL_GRIDS: ScheduleOutputPulses(x, GenerateGrids(x)); break;
    case T_GENERATOR_MODEL_DRUMS: ScheduleOutputPulses(x, GenerateDrums(x)); break;
    case T_GENERATOR_MODEL_MARKOV: ScheduleOutputPulses(x, GenerateMarkov(x)); break;
    default: 
      // Divider Logic
      --divider_pattern_length_;
      if (divider_pattern_length_ <= 0) {
        DividerPattern pattern;
        if (model_ == T_GENERATOR_MODEL_DIVIDER) pattern = bias_quantizer_.Lookup(fixed_divider_patterns, bias_);
        else {
          float u = x.variables.u[0];
          float strength = fabs(bias_ - 0.5f) * 2.0f;
          u *= (u + strength * strength * (1.0f - u)) * strength;
          pattern = divider_patterns[static_cast<size_t>(u * kNumDividerPatterns)];
          if (bias_ < 0.5f) { for (size_t i=0; i<kNumTChannels/2; ++i) swap(pattern.ratios[i], pattern.ratios[kNumTChannels-1-i]); }
        }
        for (size_t i=0; i<kNumTChannels; ++i) slave_ramp_[i].Init(pattern.length, pattern.ratios[i], RandomPulseWidth(i, x.variables.pulse_width[i]));
        divider_pattern_length_ = pattern.length;
      }
      break;
  }
}

void TGenerator::Process(bool use_external_clock, bool* reset, const GateFlags* external_clock, Ramps ramps, bool* gate, size_t size) {
  float internal_frequency;
  sample_index_ = 0;

  if (use_external_clock) {
      if (!use_external_clock_) { ramp_extractor_.Reset(); }
      Ratio ratio = rate_quantizer_.Lookup(input_divider_ratios, 1.05f * rate_ / 96.0f + 0.5f);
      if (range_ == T_GENERATOR_RANGE_0_25X) ratio.q *= 4;
      else if (range_ == T_GENERATOR_RANGE_4X) ratio.p *= 4;

      if (model_ == T_GENERATOR_MODEL_GRIDS) {
          ratio.p *= 2; 
      }

      ratio.Simplify<2>();
      ramp_extractor_.Process(ratio, true, reset, external_clock, ramps.external, size);
      internal_frequency = 0.0f;
  } else {
    float rate = 2.0f;
    if (range_ == T_GENERATOR_RANGE_4X) rate = 8.0f;
    else if (range_ == T_GENERATOR_RANGE_0_25X) rate = 0.5f;

    if (model_ == T_GENERATOR_MODEL_GRIDS) {
      rate *= 2.0f; 
    }
    internal_frequency = rate * one_hertz_ * SemitonesToRatio(rate_);
  }
  
  use_external_clock_ = use_external_clock;
  if (*reset) {
      for (size_t i = 0; i < kNumTChannels; ++i) slave_ramp_[i].Reset();
      sequence_.Reset();
      drum_pattern_step_ = 0; // CRITICAL RESET
      if (model_ != T_GENERATOR_MODEL_DIVIDER) {
          RandomVector rv;
          sequence_.NextVector(rv.x, sizeof(rv.x) / sizeof(float));
          ConfigureSlaveRamps(rv);
      }
  }
  
  while (size--) {
      float frequency = use_external_clock ? *ramps.external - previous_external_ramp_value_ : internal_frequency;
      frequency += frequency < 0.0f ? 1.0f : 0.0f;
      float j_mult = (model_ == T_GENERATOR_MODEL_GRIDS) ? 1.0f : jitter_multiplier_;
      float jittery_freq = frequency * j_mult;
      master_phase_ += jittery_freq;

      if (frequency > 0.0f) {
        current_period_ = 1.0f / frequency;
      }
      
      if (master_phase_ > 1.0f) {
          master_phase_ -= 1.0f;
          RandomVector rv;
          sequence_.NextVector(rv.x, sizeof(rv.x) / sizeof(float));
          
          float jitter_amount = jitter_ * jitter_ * jitter_ * jitter_ * 36.0f;
          float x = FastBetaDistributionSample(rv.variables.jitter);
          float mult = SemitonesToRatio((x * 2.0f - 1.0f) * jitter_amount);
          mult *= (phase_difference_ > 0.0f) ? (1.0f + phase_difference_) : (1.0f / (1.0f - phase_difference_));
          jitter_multiplier_ = mult;
          
          ConfigureSlaveRamps(rv);
      }
      if (internal_frequency) *ramps.external = master_phase_;
      previous_external_ramp_value_ = *ramps.external;
      ramps.external++;

      float output_phase = master_phase_;
      if (model_ == T_GENERATOR_MODEL_GRIDS) {
          hh_gate_buffer_[sample_index_] = (hh_pulse_counter_ > 0);
          if (hh_pulse_counter_ > 0) {
              hh_pulse_counter_--;
          }
      }
      sample_index_++;

      if (model_ == T_GENERATOR_MODEL_GRIDS) {
          if (hh_pulse_counter_ == 0) output_phase = 0.0f;
      }
      
      *ramps.master++ = output_phase;

      if (model_ == T_GENERATOR_MODEL_GRIDS) {
        hh_gate_buffer_[sample_index_] = (hh_pulse_counter_ > 0);
        if (hh_pulse_counter_ > 0) {
            hh_pulse_counter_--;
        }
        
        accent_gate_buffer_[sample_index_] = grids_accent_state_;
      }

      for (size_t j = 0; j < kNumTChannels; ++j) {
          slave_ramp_[j].Process(jittery_freq, ramps.slave[j], gate);
          ramps.slave[j]++;
          gate++;
      }


      sample_index_++;
  }
}

}  // namespace marbles