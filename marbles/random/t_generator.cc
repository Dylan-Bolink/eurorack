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

  hh_slave_ramp_.Init();
  accent_slave_ramp_.Init();
  
  hh_trigger_ = false;
  prev_hh_state_ = false;
  current_period_ = 0.0f;

  grids_swing_ = 0.0f;
  grids_accent_threshold_ = 192; // Default OG grids value
  grids_accent_mode_ = 3;  // All
  grids_interpolation_ = true;  // Default smooth
  accent_velocity_ = 1.0f;
  random_accent_voltage_ = 1.0f;
  for (int i = 0; i < 32; ++i) {
    accent_voltage_buffer_[i] = random_stream->GetFloat();
  }
  pending_hh_from_grids_ = false;
  pending_accent_from_grids_ = false;
  accent_triggered_ = false;

  grids_groove_offset_ = 0.0f;
  groove_delay_countdown_ = 0;
  groove_delay_for_t1_ = true;

  grids_sync_playheads_ = false;
  grids_loop_start_at_one_ = false;
  grids_free_step_ = 0;
  fill(grids_part_perturbation_, grids_part_perturbation_ + 3, 0);

  grids_loop_start_ = 0;
  prev_deja_vu_active_ = false;
  fill(&grids_step_replacement_[0], &grids_step_replacement_[32], 0xFF);
  drift_order_head_ = 0;
  drift_order_count_ = 0;

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

  if (model_ == T_GENERATOR_MODEL_GRIDS) {
    hh_slave_ramp_.Init(
        pending_hh_from_grids_ ? 1 : 0,
        RandomPulseWidth(0, x.variables.pulse_width[0]),
        0.5f);
    
    accent_slave_ramp_.Init(
        pending_accent_from_grids_ ? 1 : 0,
        RandomPulseWidth(0, x.variables.pulse_width[0]),
        0.5f);
  }
}

int TGenerator::GenerateGrids(const RandomVector& x) {
  ++drum_pattern_step_;
  ++grids_free_step_;
  if (grids_free_step_ >= 32) grids_free_step_ = 0;
  size_t len = static_cast<size_t>(grids_length_);
  if (len < 1) len = 32;

  const float kDead = 0.03f;
  bool kick_trig = false;
  bool snare_trig = false;
  bool hh_trig = false;
  bool kick_accent = false;
  bool snare_accent = false;
  bool hh_accent = false;

  // Left loop shifting
  bool loop_wrapped = false;
  if (prev_deja_vu_active_ && len < 32) {
    size_t end_point = (grids_loop_start_ + len) % 32;

    if (grids_loop_start_ < end_point) {
      if (drum_pattern_step_ >= end_point) {
        drum_pattern_step_ = grids_loop_start_;
        loop_wrapped = true;
      }
    } else {
      if (drum_pattern_step_ >= 32) {
        drum_pattern_step_ = 0;
      }
      if (drum_pattern_step_ >= end_point && drum_pattern_step_ < grids_loop_start_) {
        drum_pattern_step_ = grids_loop_start_;
        loop_wrapped = true;
      }
    }

    float deja_vu_amount = sequence_.deja_vu();

    // shift loop start on wraparound
    if (loop_wrapped && deja_vu_amount < 0.47f) {
      float shift_probability = 1.0f - deja_vu_amount / 0.47f;
      if (x.variables.u[0] < shift_probability) {
        grids_loop_start_ = (grids_loop_start_ + 1) % 32;
        drum_pattern_step_ = grids_loop_start_;
      }
    }
  } else {
    if (drum_pattern_step_ >= 32) drum_pattern_step_ = 0;
  }

  uint8_t read_step = drum_pattern_step_ % 32;
  uint8_t x_u8, y_u8;

  if (grids_interpolation_) {
    // Smooth interpolation
    x_u8 = static_cast<uint8_t>(grids_x_ * 255.0f);
    y_u8 = static_cast<uint8_t>(grids_y_ * 255.0f);
  } else {
    // Discrete 5x5 grid quantization (values: 0, 64, 128, 192, 255)
    uint8_t x_idx = static_cast<uint8_t>(grids_x_ * 4.99f);
    uint8_t y_idx = static_cast<uint8_t>(grids_y_ * 4.99f);
    if (x_idx > 4) x_idx = 4;
    if (y_idx > 4) y_idx = 4;
    x_u8 = x_idx < 4 ? x_idx * 64 : 255;
    y_u8 = y_idx < 4 ? y_idx * 64 : 255;
  }

  // Parse groove offset into step offset or micro-timing
  int kick_step_offset = 0, snare_step_offset = 0;
  float kick_micro = 0.0f, snare_micro = 0.0f;
  if (grids_groove_offset_ < -0.06f) {
    float amt = -grids_groove_offset_;
    if (amt >= 0.87f) kick_step_offset = 3;
    else if (amt >= 0.73f) kick_step_offset = 2;
    else if (amt >= 0.60f) kick_step_offset = 1;
    else kick_micro = (amt - 0.06f) / 0.54f * 0.5f;
  } else if (grids_groove_offset_ > 0.06f) {
    float amt = grids_groove_offset_;
    if (amt >= 0.87f) snare_step_offset = 3;
    else if (amt >= 0.73f) snare_step_offset = 2;
    else if (amt >= 0.60f) snare_step_offset = 1;
    else snare_micro = (amt - 0.06f) / 0.54f * 0.5f;
  }

  uint8_t kick_read = (read_step - kick_step_offset + 32) % 32;
  uint8_t snare_read = (read_step - snare_step_offset + 32) % 32;

  uint8_t chaos_amt = (grids_chaos_ > 0.0f)
      ? static_cast<uint8_t>(grids_chaos_ * 64.0f)
      : 0;

  //read_step normal 32-step cycle wrap
  //loop_wrapped loop boundary when locked loop
  // chaos_amt == 0 immediate reset when chaos is turned off
  if (read_step == 0 || loop_wrapped || chaos_amt == 0) {
    grids_part_perturbation_[0] = static_cast<uint8_t>(x.variables.u[0] * chaos_amt);
    grids_part_perturbation_[1] = static_cast<uint8_t>(x.variables.u[1] * chaos_amt);
    grids_part_perturbation_[2] = static_cast<uint8_t>(
        (x.variables.u[0] + x.variables.u[1]) * 0.5f * chaos_amt);
  }
  uint8_t* p = grids_part_perturbation_;

  // Right density drifting
  int8_t density_drift = 0;

  if (prev_deja_vu_active_ && len < 32) {
    float deja_vu_amount = sequence_.deja_vu();

    if (deja_vu_amount > 0.53f) {
      float drift_probability = ((deja_vu_amount - 0.53f) / 0.47f) * 0.50f;

      if (x.variables.u[0] < drift_probability) {
        size_t max_drifts = len / 2;
        if (max_drifts < 1) max_drifts = 1;

        size_t drift_count = 0;
        for (size_t i = 0; i < 32; ++i) {
          if (grids_step_replacement_[i] != 0xFF) drift_count++;
        }

        // If at max, remove one using round-robin
        if (drift_count >= max_drifts) {
          for (size_t j = 0; j < 32; ++j) {
            size_t idx = (drift_order_head_ + j) % 32;
            if (grids_step_replacement_[idx] != 0xFF && idx != read_step) {
              grids_step_replacement_[idx] = 0xFF;
              drift_order_head_ = (idx + 1) % 32;
              break;
            }
          }
        }

        // Set drift range: ±126 (~50% of threshold range)
        int8_t drift = static_cast<int8_t>((x.variables.u[1] - 0.5f) * 252.0f);
        grids_step_replacement_[read_step] = static_cast<uint8_t>(128 + drift);
      }
    }

    // Apply stored drift
    if (grids_step_replacement_[read_step] != 0xFF) {
      density_drift = static_cast<int8_t>(grids_step_replacement_[read_step]) - 128;
    }
  }

  uint8_t kick_lvl = 0, snare_lvl = 0, hh_lvl = 0;

  if (dens_kick_ > kDead) {
    kick_lvl = grids_.ReadDrumMap(kick_read, 0, x_u8, y_u8);
    if (kick_lvl < (255 - p[0])) kick_lvl += p[0]; else kick_lvl = 255;
    int thresh_i = static_cast<int>((1.0f - dens_kick_) * 255.0f) + density_drift;
    if (thresh_i < 0) thresh_i = 0;
    if (thresh_i > 255) thresh_i = 255;
    kick_trig = (kick_lvl > thresh_i);
    kick_accent = kick_trig && (kick_lvl > grids_accent_threshold_);
  }

  if (dens_snare_ > kDead) {
    snare_lvl = grids_.ReadDrumMap(snare_read, 1, x_u8, y_u8);
    if (snare_lvl < (255 - p[1])) snare_lvl += p[1]; else snare_lvl = 255;
    int thresh_i = static_cast<int>((1.0f - dens_snare_) * 255.0f) + density_drift;
    if (thresh_i < 0) thresh_i = 0;
    if (thresh_i > 255) thresh_i = 255;
    snare_trig = (snare_lvl > thresh_i);
    snare_accent = snare_trig && (snare_lvl > grids_accent_threshold_);
  }

  if (dens_hh_ > kDead) {
    hh_lvl = grids_.ReadDrumMap(read_step, 2, x_u8, y_u8);
    if (hh_lvl < (255 - p[2])) hh_lvl += p[2]; else hh_lvl = 255;
    int thresh_i = static_cast<int>((1.0f - dens_hh_) * 255.0f) + density_drift;
    if (thresh_i < 0) thresh_i = 0;
    if (thresh_i > 255) thresh_i = 255;
    hh_trig = (hh_lvl > thresh_i);
    hh_accent = hh_trig && (hh_lvl > grids_accent_threshold_);
  }

  // mirrors Process() logic for capped swing + microtiming
  float swing_fraction = 0.0f;
  if (grids_swing_ != 0.0f) {
    float swing_amount = fabsf(grids_swing_);
    bool step_is_swung;
    if (grids_swing_ < 0.0f) {
      step_is_swung = (drum_pattern_step_ & 2);  // pair swing
    } else {
      step_is_swung = ((drum_pattern_step_ % 3) == 2);  // triplet
    }
    if (step_is_swung) {
      swing_fraction = swing_amount * 0.5f;
    }
  }

  // Cap micro-timing so swing + micro <= 75% of step period
  const float kMaxCombined = 0.75f;
  float micro_headroom = kMaxCombined - swing_fraction;
  if (micro_headroom < 0.0f) micro_headroom = 0.0f;

  // Micro-timing suppress: delay the trigger by a fraction of the period
  bool kick_suppress = false, snare_suppress = false;
  if (kick_micro > 0.0f && kick_trig && current_period_ > 0.0f) {
    float capped = kick_micro < micro_headroom ? kick_micro : micro_headroom;
    kick_suppress = true;
    groove_delay_for_t1_ = true;
    groove_delay_countdown_ = static_cast<int>(capped * current_period_);
    if (groove_delay_countdown_ < 1) groove_delay_countdown_ = 1;
  }
  if (snare_micro > 0.0f && snare_trig && current_period_ > 0.0f) {
    float capped = snare_micro < micro_headroom ? snare_micro : micro_headroom;
    snare_suppress = true;
    groove_delay_for_t1_ = false;
    groove_delay_countdown_ = static_cast<int>(capped * current_period_);
    if (groove_delay_countdown_ < 1) groove_delay_countdown_ = 1;
  }

  int bitmask = 0;
  if (kick_trig && !kick_suppress) bitmask |= 1;
  if (snare_trig && !snare_suppress) bitmask |= 2;

  // Accent mode: 0=kick only, 1=hh only, 2=snare only, 3=all
  bool combined_accent;
  switch (grids_accent_mode_) {
    case 0: combined_accent = kick_accent; break;
    case 1: combined_accent = hh_accent; break;
    case 2: combined_accent = snare_accent; break;
    default: combined_accent = kick_accent || snare_accent || hh_accent; break;
  }

  // Calculate velocity for dynamic accent mode
  if (combined_accent) {
    uint8_t accent_level = 0;
    switch (grids_accent_mode_) {
      case 0: accent_level = kick_lvl; break;
      case 1: accent_level = hh_lvl; break;
      case 2: accent_level = snare_lvl; break;
      default: {
        // All mode: use highest accenting level
        if (kick_accent && kick_lvl > accent_level) accent_level = kick_lvl;
        if (snare_accent && snare_lvl > accent_level) accent_level = snare_lvl;
        if (hh_accent && hh_lvl > accent_level) accent_level = hh_lvl;
        break;
      }
    }
    // Velocity: 0.0 at threshold, 1.0 at 255
    float range = 255.0f - static_cast<float>(grids_accent_threshold_);
    if (range > 0.0f) {
      accent_velocity_ = (static_cast<float>(accent_level) - static_cast<float>(grids_accent_threshold_)) / range;
      if (accent_velocity_ < 0.0f) accent_velocity_ = 0.0f;
      if (accent_velocity_ > 1.0f) accent_velocity_ = 1.0f;
    } else {
      accent_velocity_ = 1.0f;
    }
    
    //If locked read from buffer, else store in buffer and read new random value
    if (prev_deja_vu_active_) {
      random_accent_voltage_ = accent_voltage_buffer_[read_step];
    } else {
      random_accent_voltage_ = x.variables.u[1];
      accent_voltage_buffer_[read_step] = random_accent_voltage_;
    }
  }

  pending_hh_from_grids_ = hh_trig;
  pending_accent_from_grids_ = combined_accent;
  if (combined_accent) {
    accent_triggered_ = true;
  }

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
      hh_slave_ramp_.Reset();
      accent_slave_ramp_.Reset(); 
      sequence_.Reset();
      drum_pattern_step_ = 0;
      grids_loop_start_ = 0;

      if (model_ != T_GENERATOR_MODEL_DIVIDER) {
          RandomVector rv;
          sequence_.NextVector(rv.x, sizeof(rv.x) / sizeof(float));
          ConfigureSlaveRamps(rv);
      }
  }
  
  while (size--) {
    float frequency = use_external_clock ? *ramps.external - previous_external_ramp_value_ : internal_frequency;
    frequency += frequency < 0.0f ? 1.0f : 0.0f;
    float j_mult = (model_ == T_GENERATOR_MODEL_GRIDS && grids_chaos_ >= 0.0f) ? 1.0f : jitter_multiplier_;
    float jittery_freq = frequency * j_mult;
    master_phase_ += jittery_freq;

    if (frequency > 0.0f) {
      current_period_ = 1.0f / frequency;
    }
    
    if (master_phase_ > 1.0f) {
      // Calculate swing threshold
      float swing_threshold = 1.0f;
      if (model_ == T_GENERATOR_MODEL_GRIDS && grids_swing_ != 0.0f) {
          size_t next_step = (drum_pattern_step_ + 1) % 32;
          float swing_amount = fabsf(grids_swing_);
          bool next_is_swung;
          if (grids_swing_ < 0.0f) {
              // Left: pair swing
              next_is_swung = (next_step & 2);
          } else {
              // Right: triplet swing
              next_is_swung = ((next_step % 3) == 2);
          }
          if (next_is_swung) {
            // Delay up to 50% of step duration
              swing_threshold = 1.0f + (swing_amount * 0.5f);
          }
      }

      if (master_phase_ >= swing_threshold) {
        master_phase_ -= 1.0f;  // Subtract threshold, not 1.0
        RandomVector rv;
        sequence_.NextVector(rv.x, sizeof(rv.x) / sizeof(float));

        float jitter_amount;
        if (model_ == T_GENERATOR_MODEL_GRIDS && grids_chaos_ < 0.0f) {
            float slop = fabsf(grids_chaos_);
            jitter_amount = slop * slop * slop * slop * 36.0f;
        } else {
            jitter_amount = jitter_ * jitter_ * jitter_ * jitter_ * 36.0f;
        }
        float x = FastBetaDistributionSample(rv.variables.jitter);
        float mult = SemitonesToRatio((x * 2.0f - 1.0f) * jitter_amount);
        mult *= (phase_difference_ > 0.0f) ? (1.0f + phase_difference_) : (1.0f / (1.0f - phase_difference_));
        jitter_multiplier_ = mult;
        
        ConfigureSlaveRamps(rv);
      }
    }
    
    if (internal_frequency) *ramps.external = master_phase_;
    previous_external_ramp_value_ = *ramps.external;
    ramps.external++;

    float output_phase = master_phase_;
    
    *ramps.master++ = output_phase;

    for (size_t j = 0; j < kNumTChannels; ++j) {
      slave_ramp_[j].Process(jittery_freq, ramps.slave[j], gate);
      ramps.slave[j]++;
      gate++;
    }

    if (model_ == T_GENERATOR_MODEL_GRIDS) {
      bool hh_gate;
      float hh_ramp_value;
      hh_slave_ramp_.Process(jittery_freq, &hh_ramp_value, &hh_gate);
      hh_gate_buffer_[sample_index_] = hh_gate;

      bool accent_gate;
      float accent_ramp_value;
      accent_slave_ramp_.Process(jittery_freq, &accent_ramp_value, &accent_gate);
      accent_gate_buffer_[sample_index_] = accent_gate;

      // Groove delay: retrigger slave ramp after countdown expires
      if (groove_delay_countdown_ > 0) {
        --groove_delay_countdown_;
        if (groove_delay_countdown_ == 0) {
          float pw = 0.05f + 0.9f * pulse_width_mean_;
          size_t idx = groove_delay_for_t1_ ? 0 : 1;
          slave_ramp_[idx].Init(true, pw, 0.5f);
        }
      }

      if (!hh_gate) {
        *(ramps.master - 1) = 0.0f;
      }
    }

    sample_index_++;
  }
}

}  // namespace marbles