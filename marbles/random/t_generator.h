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

#ifndef MARBLES_RANDOM_T_GENERATOR_H_
#define MARBLES_RANDOM_T_GENERATOR_H_

#include "stmlib/stmlib.h"
#include "marbles/io_buffer.h"
#include "grids/pattern_generator.h"

#include "marbles/ramp/ramp_divider.h"
#include "marbles/ramp/ramp_extractor.h"
#include "marbles/ramp/ramp_generator.h"
#include "marbles/ramp/slave_ramp.h"
#include "marbles/random/distributions.h"
#include "marbles/random/random_sequence.h"
#include "stmlib/dsp/hysteresis_quantizer.h"

namespace marbles {

enum TGeneratorModel {
  T_GENERATOR_MODEL_COMPLEMENTARY_BERNOULLI,
  T_GENERATOR_MODEL_CLUSTERS,
  T_GENERATOR_MODEL_DRUMS,

  T_GENERATOR_MODEL_INDEPENDENT_BERNOULLI,
  T_GENERATOR_MODEL_DIVIDER,
  T_GENERATOR_MODEL_GRIDS,
  
  T_GENERATOR_MODEL_MARKOV,
};

enum TGeneratorRange {
  T_GENERATOR_RANGE_0_25X,
  T_GENERATOR_RANGE_1X,
  T_GENERATOR_RANGE_4X,
};

const size_t kNumTChannels = 2;
const size_t kMarkovHistorySize = 16;
const size_t kNumDrumPatterns = 18;
const size_t kDrumPatternSize = 8;

struct DividerPattern {
  Ratio ratios[kNumTChannels];
  int32_t length;
};

struct Ramps {
  float* external;
  float* master;
  float* slave[kNumTChannels];
};

const size_t kNumDividerPatterns = 17;
const size_t kNumInputDividerRatios = 9;

class TGenerator {
 public:
  TGenerator() { }
  ~TGenerator() { }
  
  void Init(RandomStream* random_stream, float sr);

  void Process(
      bool use_external_clock,
      const stmlib::GateFlags* external_clock,
      Ramps ramps,
      bool* gate,
      size_t size) {
    bool reset = false;
    Process(use_external_clock, &reset, external_clock, ramps, gate, size);
  }

  void Process(
      bool use_external_clock,
      bool* reset,
      const stmlib::GateFlags* external_clock,
      Ramps ramps,
      bool* gate,
      size_t size);
  
  inline void set_model(TGeneratorModel model) {
    model_ = model;
  }
  
  inline void set_range(TGeneratorRange range) {
    range_ = range;
  }
  
  inline void set_rate(float rate) {
    rate_ = rate;
  }
  
  inline void set_bias(float bias) {
    bias_ = bias;
  }
  
  inline void set_jitter(float jitter) {
    jitter_ = jitter;
  }
  
  inline void set_deja_vu(float deja_vu) {
    sequence_.set_deja_vu(deja_vu);
  }

  inline void set_length(int length) {
    sequence_.set_length(length);
  }

  inline void set_pulse_width_mean(float pulse_width_mean) {
    pulse_width_mean_ = pulse_width_mean;
  }
  
  inline void set_pulse_width_std(float pulse_width_std) {
    pulse_width_std_ = pulse_width_std;
  }

  void set_grids_coordinates(float x, float y, float chaos) {
      grids_x_ = x;
      grids_y_ = y;
      grids_chaos_ = chaos;
  }
  
  void set_grids_densities(float kick, float snare, float hh) {
      float k = kick  < 0.0f ? 0.0f : (kick  > 1.0f ? 1.0f : kick);
      float s = snare < 0.0f ? 0.0f : (snare > 1.0f ? 1.0f : snare);
      float h = hh    < 0.0f ? 0.0f : (hh    > 1.0f ? 1.0f : hh);

      dens_kick_  = k;
      dens_snare_ = s;
      dens_hh_    = h;
  }

  void set_grids_length(int length) {
      grids_length_ = length;
  }

  bool get_hh_gate(size_t i) const { return hh_gate_buffer_[i]; }
  bool get_accent_gate(size_t i) const { return accent_gate_buffer_[i]; }
  float get_accent_velocity() const { return accent_velocity_; }
  float get_random_accent_voltage() const { return random_accent_voltage_; }
  float get_pulse_width_mean() const { return pulse_width_mean_; }

  bool get_and_clear_accent_triggered() {
    bool result = accent_triggered_;
    accent_triggered_ = false;
    return result;
  }

  void set_grids_swing(float swing) {
    grids_swing_ = swing;
  }

  void set_grids_accent_threshold(uint8_t threshold) {
    grids_accent_threshold_ = threshold;
  }

  // 0=kick 1=hh 2=snare 3=all
  void set_grids_accent_mode(uint8_t mode) {
    grids_accent_mode_ = mode;
  }

  void set_grids_interpolation(bool interpolation) {
    grids_interpolation_ = interpolation;
  }

  void set_grids_bank(uint8_t bank) {
    grids_.SetBank(bank);
  }

  void set_grids_groove_offset(float offset) {
    grids_groove_offset_ = offset;
  }

  void set_grids_henri(bool henri) {
    grids_.SetHenri(henri);
  }

  void set_grids_sync_playheads(bool sync) {
    grids_sync_playheads_ = sync;
  }

  void set_grids_loop_start_at_one(bool at_one) {
    grids_loop_start_at_one_ = at_one;
  }

  void set_grids_deja_vu_active(bool active, bool reset_active = false) {
    // On lock activation
    if (active && !prev_deja_vu_active_) {
      size_t len = static_cast<size_t>(grids_length_);
      grids_loop_start_ = grids_loop_start_at_one_
          ? 0
          : (drum_pattern_step_ + 33 - len) % 32;

      // Clear step drifts on new lock
      for (size_t i = 0; i < 32; ++i) {
        grids_step_replacement_[i] = 0xFF;
      }
      drift_order_head_ = 0;
      drift_order_count_ = 0;
    }

    // On unlock: sync playhead if enabled
    if (!active && prev_deja_vu_active_ && grids_sync_playheads_) {
      drum_pattern_step_ = grids_free_step_;
    }

    // Explicit reset: jump to loop start (or 0 if no loop)
    if (reset_active) {
      if (active) {
        drum_pattern_step_ = (grids_loop_start_ + 31) % 32;
      } else {
        drum_pattern_step_ = 31;
        grids_free_step_ = 31;
      }
    }

    prev_deja_vu_active_ = active;
  }

  void reset_grids_loop_start() {
    grids_loop_start_ = 0;
  }

  
 private:
  union RandomVector {
    struct {
      float pulse_width[kNumTChannels];
      float u[kNumTChannels];
      float p;
      float jitter;
    } variables;
    float x[2 * kNumTChannels + 2];
  };
  
  void ConfigureSlaveRamps(const RandomVector& v);
  int GenerateComplementaryBernoulli(const RandomVector& v);
  int GenerateIndependentBernoulli(const RandomVector& v);
  int GenerateGrids(const RandomVector& x);
  int GenerateDrums(const RandomVector& v);
  int GenerateMarkov(const RandomVector& v);
  void ScheduleOutputPulses(const RandomVector& v, int bitmask);

  float RandomPulseWidth(int i, float u) {
    if (pulse_width_std_ == 0.0f) {
      return 0.05f + 0.9f * pulse_width_mean_;
    } else {
      return 0.05f + 0.9f * BetaDistributionSample(
          u,
          pulse_width_std_,
          pulse_width_mean_);  // Jon Brooks
          // i & 1 ? 1.0f - pulse_width_mean_);
    }
  }
  
  float one_hertz_;

  TGeneratorModel model_;
  TGeneratorRange range_;
  
  float rate_;
  float bias_;
  float jitter_;
  float pulse_width_mean_;
  float pulse_width_std_;
  
  float master_phase_;
  float jitter_multiplier_;
  float phase_difference_;
  float previous_external_ramp_value_;

  grids::PatternGenerator grids_;
  
  float grids_x_, grids_y_, grids_chaos_;
  float dens_kick_, dens_snare_, dens_hh_;
  int grids_length_;
  bool hh_trigger_;
  bool prev_hh_state_;
  float current_period_;

  bool hh_gate_buffer_[kBlockSize];
  bool accent_gate_buffer_[kBlockSize];
  size_t sample_index_;

  float grids_swing_;
  float grids_swing_latched_;
  uint8_t grids_accent_threshold_;
  uint8_t grids_accent_mode_;  // 0=kick, 1=hh, 2=snare, 3=all
  bool grids_interpolation_;
  float accent_velocity_;
  float random_accent_voltage_;
  float accent_voltage_buffer_[32];
  bool pending_hh_from_grids_;
  bool pending_accent_from_grids_;
  bool accent_triggered_;

  // Groove offset: -1 to +1, negative = kick delayed, positive = snare delayed
  float grids_groove_offset_;
  int groove_delay_countdown_;
  bool groove_delay_for_t1_;  // true = kick (T1/slave_ramp_[0]), false = snare (T3/slave_ramp_[1])

  SlaveRamp hh_slave_ramp_;
  SlaveRamp accent_slave_ramp_;

  bool grids_sync_playheads_;
  uint8_t grids_loop_start_at_one_;
  uint8_t grids_free_step_;  // shadow counter for sync playheads
  uint8_t grids_part_perturbation_[3];  // per-instrument chaos, set at step 0

  size_t grids_loop_start_;
  bool prev_deja_vu_active_;
  uint8_t grids_step_replacement_[32];

  uint8_t drift_order_[8];
  size_t drift_order_head_;
  size_t drift_order_count_;

  bool use_external_clock_;

  int32_t divider_pattern_length_;
  int32_t streak_counter_[kMarkovHistorySize];
  int32_t markov_history_[kMarkovHistorySize];
  int32_t markov_history_ptr_;
  size_t drum_pattern_step_;
  size_t drum_pattern_index_;

  RandomSequence sequence_;
  RampExtractor ramp_extractor_;
  RampGenerator ramp_generator_;

  SlaveRamp slave_ramp_[kNumTChannels];
  
  stmlib::HysteresisQuantizer2 bias_quantizer_;
  stmlib::HysteresisQuantizer2 rate_quantizer_;
  
  static DividerPattern divider_patterns[kNumDividerPatterns];
  static DividerPattern fixed_divider_patterns[kNumDividerPatterns];
  static Ratio input_divider_ratios[kNumInputDividerRatios];
  static uint8_t drum_patterns[kNumDrumPatterns][kDrumPatternSize];
  
  DISALLOW_COPY_AND_ASSIGN(TGenerator);
};

}  // namespace marbles

#endif  // MARBLES_RANDOM_T_GENERATOR_H_
