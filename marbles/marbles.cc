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

#include <stm32f4xx_conf.h>

#include "marbles/drivers/clock_inputs.h"
#include "marbles/drivers/dac.h"
#include "marbles/drivers/debug_pin.h"
#include "marbles/drivers/debug_port.h"
#include "marbles/drivers/gate_outputs.h"
#include "marbles/drivers/rng.h"
#include "marbles/drivers/system.h"

#include "marbles/ramp/ramp_extractor.h"
#include "marbles/random/random_generator.h"
#include "marbles/random/random_stream.h"
#include "marbles/random/t_generator.h"
#include "marbles/random/x_y_generator.h"

#include "marbles/clock_self_patching_detector.h"
#include "marbles/cv_reader.h"
#include "marbles/io_buffer.h"
#include "marbles/note_filter.h"
#include "marbles/resources.h"
#include "marbles/scale_recorder.h"
#include "marbles/settings.h"
#include "marbles/ui.h"

#include "stmlib/dsp/dsp.h"
#include "stmlib/dsp/hysteresis_quantizer.h"
#include "stmlib/dsp/units.h"

#define PROFILE_INTERRUPT 0
#define PROFILE_RENDER 0

using namespace marbles;
using namespace std;
using namespace stmlib;

const bool test_adc_noise = false;

const int kSampleRate = 32000;
const int kGateDelay = 2;
const float kDeadband = 0.06f;

float grids_chaos = 0.0f;
int grids_length_ = 32;

ClockInputs clock_inputs;
ClockSelfPatchingDetector self_patching_detector[kNumGateOutputs];
CvReader cv_reader;
Dac dac;
DebugPort debug_port;
GateOutputs gate_outputs;
HysteresisQuantizer2 deja_vu_length_quantizer;
IOBuffer io_buffer;
NoteFilter note_filter;
Rng rng;
ScaleRecorder scale_recorder;
Settings settings;
Ui ui;

RandomGenerator random_generator;
RandomStream random_stream;
TGenerator t_generator;
XYGenerator xy_generator;

// Default interrupt handlers.
extern "C" {

int __errno;

void NMI_Handler() { }
void HardFault_Handler() { while (1); }
void MemManage_Handler() { while (1); }
void BusFault_Handler() { while (1); }
void UsageFault_Handler() { while (1); }
void SVC_Handler() { }
void DebugMon_Handler() { }
void PendSV_Handler() { }

void SysTick_Handler() {
  IWDG_ReloadCounter();
  ui.Poll();
  if (settings.freshly_baked()) {
    if (debug_port.readable()) {
      uint8_t command = debug_port.Read();
      uint8_t response = ui.HandleFactoryTestingRequest(command);
      debug_port.Write(response);
    }
  }
}

}

IOBuffer::Slice FillBuffer(size_t size) {
  if (PROFILE_INTERRUPT) {
    TIC;
  }
  IOBuffer::Slice s = io_buffer.NextSlice(size);
  
  gate_outputs.Write(s);
  clock_inputs.Read(s, size);

  if (io_buffer.new_block()) {
    cv_reader.Copy(&s.block->adc_value[0]);
    clock_inputs.ReadNormalization(s.block);
  }

  if (rng.readable()) {
    random_stream.Write(rng.data());
  }

  if (PROFILE_INTERRUPT) {
    TOC;
  }
  
  return s;
}

inline uint16_t DacCode(int index, float voltage) {
  CONSTRAIN(voltage, -5.0f, 5.0f);
  const float scale = settings.calibration_data().dac_scale[index];
  const float offset = settings.calibration_data().dac_offset[index];
  return ClipU16(static_cast<int32_t>(voltage * scale + offset));
}

void ProcessTest(IOBuffer::Block* block, size_t size) {
  float parameters[kNumParameters];
  GateFlags hidden_gates[kNumParameters];
  static float phase;
  cv_reader.Process(false, &block->adc_value[0], parameters, hidden_gates);
  for (size_t i = 0; i < size; ++i) {
    phase += 100.0f / static_cast<float>(kSampleRate);
    if (phase >= 1.0f) {
      phase -= 1.0f;
    }
    block->cv_output[0][i] = DacCode(
        0, 4.0 * Interpolate(lut_sine, phase, 256.0f));
    block->cv_output[1][i] = DacCode(
        1, -8.0f * phase + 4.0f);
    block->cv_output[2][i] = DacCode(
        2, (phase < 0.5f ? phase : 1.0f - phase) * 16.0f - 4.0f);
    block->cv_output[3][i] = DacCode(
        3, phase < 0.5f ? -4.0f : 4.0f);

    for (int j = 0; j < 4; ++j) {
      uint16_t dac_code = ui.output_test_forced_dac_code(j);
      if (dac_code) {
        block->cv_output[j][i] = dac_code;
      }
    }
    
    block->gate_output[0][i] = block->input_patched[0]
        ? block->input[0][i]
        : phase < 0.2f;
    block->gate_output[1][i] = phase < 0.5f;
    block->gate_output[2][i] = block->input_patched[1]
        ? block->input[1][i]
        : phase < 0.8f;
  }
}

Ratio y_divider_ratios[] = {
  { 1, 64 },
  { 1, 48 },
  { 1, 32 },
  { 1, 24 },
  { 1, 16 },
  { 1, 12 },
  { 1, 8 },
  { 1, 6 },
  { 1, 4 },
  { 1, 3 },
  { 1, 2 },
  { 1, 1 },
};

int loop_length[] = {
  1,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
  5, 5, 5, 5,
  6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
  7, 7,
  8, 8, 8, 8, 8, 8, 8, 8, 8,
  10, 10, 10,
  12, 12, 12, 12, 12, 12, 12,
  14, 14,
  16
};
GateFlags hidden_gates[kNumParameters];
float parameters[kNumParameters];
float ramp_buffer[kBlockSize * 4];
bool gates[kBlockSize * 2];
float voltages[kBlockSize * 4];
Ramps ramps;
GroupSettings x, y;
bool gate_delay_tail[kNumGateOutputs][kGateDelay];
bool accent_cv_delay_tail[kGateDelay] = {false, false};
float held_accent_voltage = 0.0f;

float SineOscillator(float voltage) {
  static float phase = 0.0f;
  CONSTRAIN(voltage, -5.0f, 5.0f);
  float frequency = stmlib::SemitonesToRatio(voltage * 12.0f) * 220.0f / kSampleRate;
  phase += frequency;
  if (phase >= 1.0f) {
    phase -= 1.0f;
  }
  return 5.0f * Interpolate(lut_sine, phase, 256.0f);
}

void Process(IOBuffer::Block* block, size_t size) {
  if (PROFILE_RENDER) {
    TIC;
  }

  // Filter CV values (3.5%)
  cv_reader.Process(
      settings.explicit_reset(),
      &block->adc_value[0],
      parameters,
      hidden_gates);

  const State& state = settings.state();

  bool grids_mode = (state.t_model == T_GENERATOR_MODEL_GRIDS);
  // Envelope mode ignores the X register modes entirely (see the X-section
  // plumbing); routing decisions must agree with that, or a leftover
  // Transpose/External setting silently deadens the Spread jack while the
  // X Ext LED claims register modes are off.
  bool x_register_active = state.x_register_mode != X_REGISTER_MODE_OFF
      && state.x_control_mode != CONTROL_MODE_ENVELOPE;
  float deja_vu = parameters[ADC_CHANNEL_DEJA_VU_AMOUNT];

  uint8_t t_deja_vu_state = state.t_deja_vu;
  uint8_t x_deja_vu_state = state.x_deja_vu;

  if (grids_mode && (settings.deja_vu_t_cv_swap() || settings.deja_vu_x_cv_swap())) {
    bool gate_high = hidden_gates[ADC_CHANNEL_DEJA_VU_AMOUNT] & GATE_FLAG_HIGH;

    if (settings.deja_vu_t_cv_swap() && gate_high) {
      if (state.t_deja_vu == DEJA_VU_ON || state.t_deja_vu == DEJA_VU_LOCKED) {
        t_deja_vu_state = DEJA_VU_OFF;
      } else {
        t_deja_vu_state = DEJA_VU_ON;
      }
    }
    if (settings.deja_vu_x_cv_swap() && gate_high) {
      if (state.x_deja_vu == DEJA_VU_ON || state.x_deja_vu == DEJA_VU_LOCKED) {
        x_deja_vu_state = DEJA_VU_OFF;
      } else {
        x_deja_vu_state = DEJA_VU_ON;
      }
    }
  }


  //  Deadband near 12 o'clock for the deja vu parameter.
  const float d = fabsf(deja_vu - 0.5f);
  if (d > 0.03f) {
    ui.set_deja_vu_lock(false);
  } else if (d < 0.02f) {
    ui.set_deja_vu_lock(true);
  }
  if (deja_vu < 0.47f) {
    deja_vu *= 1.06382978723f;
  } else if (deja_vu > 0.53f) {
    deja_vu = 0.5f + (deja_vu - 0.53f) * 1.06382978723f;
  } else {
    deja_vu = 0.5f;
  }
  
  GateFlags* t_clock = block->input[0];
  GateFlags* xy_clock = block->input[1];
  
  // Determine the clock source for the XY section (2%)
  ClockSource xy_clock_source = CLOCK_SOURCE_INTERNAL_T1_T2_T3;
  if (block->input_patched[1]) {
    xy_clock_source = CLOCK_SOURCE_EXTERNAL;
    size_t best_score = 8;
    for (size_t i = 0; i < kNumGateOutputs; ++i) {
      size_t score = self_patching_detector[i].Process(block, size);
      if (score >= best_score) {
        xy_clock_source = ClockSource(CLOCK_SOURCE_INTERNAL_T1 + i);
        best_score = score;
      }
    }
  }
  
  // Generate gates for T-section (16%).
  ramps.master = &ramp_buffer[0];
  ramps.external = &ramp_buffer[kBlockSize];
  ramps.slave[0] = &ramp_buffer[kBlockSize * 2];
  ramps.slave[1] = &ramp_buffer[kBlockSize * 3];

  int deja_vu_length = deja_vu_length_quantizer.Lookup(
      loop_length,
      parameters[ADC_CHANNEL_DEJA_VU_LENGTH]);
  

  // Disable if grids_y_cv_swap == 2 (jitter CV goes to map Y)
  bool t_reset_cv_available = !(grids_mode && settings.grids_y_cv_swap() == 2);
  bool t_section_reset = (settings.explicit_reset() & 1) && t_reset_cv_available &&
      hidden_gates[ADC_CHANNEL_T_JITTER] & GATE_FLAG_RISING;
  
  t_generator.set_model(TGeneratorModel(state.t_model));
  t_generator.set_range(TGeneratorRange(state.t_range));
  
  float k_bias_t = parameters[ADC_CHANNEL_T_BIAS];
  float k_jitter = parameters[ADC_CHANNEL_T_JITTER];

  if (grids_mode) {
    float swing_raw = static_cast<float>(state.grids_swing) / 255.0f;
    float swing = (swing_raw - 0.5f) * 2.0f;
    if (fabsf(swing) < kDeadband) swing = 0.0f; // deadband check
    t_generator.set_grids_swing(swing);

    // left = channel select, center = all @ 192, right = lower threshold
    uint8_t knob = state.grids_accent_threshold;
    uint8_t accent_mode = 3;  // default: all
    uint8_t accent_thresh = 192;

    if (knob < 32) {
      accent_mode = 0;  // kick 
    } else if (knob < 64) {
      accent_mode = 1;  // hh 
    } else if (knob < 96) {
      accent_mode = 2;  // snare 
    } else if (knob < 160) {
      accent_mode = 3;  // all
    } else {
      // lower threshold
      accent_mode = 3;
      int thresh = 192 - static_cast<int>(knob - 160) * 184 / 95;
      if (thresh < 8) thresh = 8;
      accent_thresh = static_cast<uint8_t>(thresh);
    }

    t_generator.set_grids_accent_mode(accent_mode);
    t_generator.set_grids_accent_threshold(accent_thresh);
    t_generator.set_grids_interpolation(state.grids_interpolation);
    t_generator.set_grids_bank(state.grids_bank);
    t_generator.set_grids_henri(state.grids_henri);
    t_generator.set_grids_sync_playheads(state.grids_sync_playheads);
    t_generator.set_grids_loop_start_at_one(state.grids_loop_start_at_one);
    // Acid clocks off the clean master ramp, not the hi-hat-gated one.
    t_generator.set_acid_active(state.x_control_mode == CONTROL_MODE_ACID);

    float groove_raw = static_cast<float>(state.grids_groove_offset) / 255.0f;
    float groove = (groove_raw - 0.5f) * 2.0f;
    if (fabsf(groove) < kDeadband) groove = 0.0f;
    t_generator.set_grids_groove_offset(groove);

    float rate_normalized = static_cast<float>(state.t_rate_stored) / 255.0f;
    t_generator.set_rate(rate_normalized * 120.0f - 60.0f);
    float final_x, final_y, final_chaos;
    if (state.grids_knob_swap) {
      // Swapped: grids coords from pot only (CV stays with X params)
      final_x = cv_reader.channel(ADC_CHANNEL_X_STEPS).unscaled_stored_pot();
      final_y = cv_reader.channel(ADC_CHANNEL_X_BIAS).unscaled_stored_pot();
      float chaos_pot = cv_reader.channel(ADC_CHANNEL_X_SPREAD).unscaled_stored_pot();
      final_chaos = (chaos_pot - 0.5f) * 2.0f;

      // CV swap still routes CV to grids coords when explicitly enabled
      if (settings.grids_x_cv_swap() == 1) {
        final_x += cv_reader.channel(ADC_CHANNEL_X_STEPS).cv();
        CONSTRAIN(final_x, 0.0f, 1.0f);
      } else if (settings.grids_x_cv_swap() == 2) {
        final_x += cv_reader.channel(ADC_CHANNEL_T_BIAS).cv();
        CONSTRAIN(final_x, 0.0f, 1.0f);
      }
      if (settings.grids_y_cv_swap() == 1) {
        final_y += cv_reader.channel(ADC_CHANNEL_X_BIAS).cv();
        CONSTRAIN(final_y, 0.0f, 1.0f);
      } else if (settings.grids_y_cv_swap() == 2) {
        final_y += cv_reader.channel(ADC_CHANNEL_T_JITTER).cv();
        CONSTRAIN(final_y, 0.0f, 1.0f);
      }
      if (settings.grids_chaos_cv_swap() == 1 && !x_register_active) {
        final_chaos += cv_reader.channel(ADC_CHANNEL_X_SPREAD).cv();
        CONSTRAIN(final_chaos, -1.0f, 1.0f);
      } else if (settings.grids_chaos_cv_swap() == 2) {
        final_chaos += cv_reader.channel(ADC_CHANNEL_T_RATE).cv() / 60.0f;
        CONSTRAIN(final_chaos, -1.0f, 1.0f);
      }
    } else {
      // Normal: grids coords from stored alt fields
      final_x = static_cast<float>(state.x_steps_alt) / 255.0f;
      final_y = static_cast<float>(state.x_bias_alt) / 255.0f;
      float chaos_raw = static_cast<float>(state.x_spread_alt) / 255.0f;
      final_chaos = (chaos_raw - 0.5f) * 2.0f;

      // Map X CV: 0=off, 1=steps, 2=t_bias
      if (settings.grids_x_cv_swap() == 1) {
        final_x += cv_reader.channel(ADC_CHANNEL_X_STEPS).cv();
        CONSTRAIN(final_x, 0.0f, 1.0f);
      } else if (settings.grids_x_cv_swap() == 2) {
        final_x += cv_reader.channel(ADC_CHANNEL_T_BIAS).cv();
        CONSTRAIN(final_x, 0.0f, 1.0f);
      }

      // Map Y CV: 0=off, 1=x_bias, 2=jitter
      if (settings.grids_y_cv_swap() == 1) {
        final_y += cv_reader.channel(ADC_CHANNEL_X_BIAS).cv();
        CONSTRAIN(final_y, 0.0f, 1.0f);
      } else if (settings.grids_y_cv_swap() == 2) {
        final_y += cv_reader.channel(ADC_CHANNEL_T_JITTER).cv();
        CONSTRAIN(final_y, 0.0f, 1.0f);
      }

      // Chaos CV: 0=off, 1=spread, 2=rate
      if (settings.grids_chaos_cv_swap() == 1 && !x_register_active) {
        final_chaos += cv_reader.channel(ADC_CHANNEL_X_SPREAD).cv();
        CONSTRAIN(final_chaos, -1.0f, 1.0f);
      } else if (settings.grids_chaos_cv_swap() == 2) {
        final_chaos += cv_reader.channel(ADC_CHANNEL_T_RATE).cv() / 60.0f;
        CONSTRAIN(final_chaos, -1.0f, 1.0f);
      }
    }
    if (fabsf(final_chaos) < kDeadband) final_chaos = 0.0f;

    float hh_density = static_cast<float>(state.grids_hh_density) / 255.0f;
    if (settings.grids_chaos_cv_swap() != 2) {
        hh_density += cv_reader.channel(ADC_CHANNEL_T_RATE).cv() / 60.0f;
    }
    CONSTRAIN(hh_density, 0.0f, 1.0f);

    float kick_density;
    if (settings.grids_x_cv_swap() == 2) {
        kick_density = cv_reader.channel(ADC_CHANNEL_T_BIAS).unscaled_pot();
    } else {
        kick_density = parameters[ADC_CHANNEL_T_BIAS];
    }

    float snare_density;
    if (settings.grids_y_cv_swap() == 2) {
        snare_density = cv_reader.channel(ADC_CHANNEL_T_JITTER).unscaled_pot();
    } else {
        snare_density = parameters[ADC_CHANNEL_T_JITTER];
    }

    // Momentary drum fill (T Range held): a randomly-chosen flavor overrides
    // the drum-map coordinates / densities while active, then snaps back.
    bool snare_roll = false;
    if (ui.drum_fill_active()) {
      switch (ui.drum_fill_flavor()) {
        case 0: {  // Build: staggered, arrangement-style ramp over the hold.
          // Hats densify first, snare joins mid-hold, kick + chaos arrive
          // last. Moderate peaks: a climax, not a wall of triggers. Voices
          // already set above their peak are left alone (never pulled down).
          float ramp = ui.drum_fill_ramp();
          float hh_ramp = ramp * 2.0f;
          float snare_ramp = ramp * 2.0f - 0.5f;
          float kick_ramp = ramp * 2.0f - 1.0f;
          CONSTRAIN(hh_ramp, 0.0f, 1.0f);
          CONSTRAIN(snare_ramp, 0.0f, 1.0f);
          CONSTRAIN(kick_ramp, 0.0f, 1.0f);
          if (hh_density < 0.85f) {
            hh_density += (0.85f - hh_density) * hh_ramp;
          }
          if (snare_density < 0.8f) {
            snare_density += (0.8f - snare_density) * snare_ramp;
          }
          if (kick_density < 0.75f) {
            kick_density += (0.75f - kick_density) * kick_ramp;
          }
          if (final_chaos < 0.45f) {
            final_chaos += (0.45f - final_chaos) * kick_ramp;
          }
          break;
        }
        case 1:  // Roll: snare on every step, kick/hats pulled back.
          snare_roll = true;
          snare_density = 1.0f;
          kick_density = 0.3f;
          hh_density = 0.25f;
          final_chaos = 0.0f;
          break;
        default: {  // Scatter: jump to another map spot, moderately busy.
          // Random offset of 0.25..0.75 (wrapped) instead of an absolute
          // target, so the fill always lands a real distance away and never
          // degenerates into "same groove" when the target is nearby.
          final_x += 0.25f + 0.5f * static_cast<float>(ui.drum_fill_x()) / 255.0f;
          if (final_x >= 1.0f) final_x -= 1.0f;
          final_y += 0.25f + 0.5f * static_cast<float>(ui.drum_fill_y()) / 255.0f;
          if (final_y >= 1.0f) final_y -= 1.0f;
          final_chaos = 0.0f;
          kick_density = snare_density = hh_density = 0.7f;
          break;
        }
      }
    }
    t_generator.set_grids_snare_roll(snare_roll);

    t_generator.set_grids_coordinates(final_x, final_y, final_chaos);

    t_generator.set_grids_densities(
      kick_density,
      snare_density,
      hh_density
    );

    bool deja_vu_active = (t_deja_vu_state == DEJA_VU_ON || t_deja_vu_state == DEJA_VU_LOCKED);
    if(deja_vu_active) {
      t_generator.set_grids_length(deja_vu_length);
    } else {
      t_generator.set_grids_length(grids_length_);
    }

    t_generator.set_grids_deja_vu_active(deja_vu_active, t_section_reset);

  } else {
    // Marbles modes
    float k_rate = parameters[ADC_CHANNEL_T_RATE];
    t_generator.set_rate(k_rate);
    t_generator.set_bias(k_bias_t);
    t_generator.set_jitter(k_jitter);
  }

  t_generator.set_deja_vu(t_deja_vu_state == DEJA_VU_LOCKED ? 0.5f : (t_deja_vu_state == DEJA_VU_ON ? deja_vu : 0.0f));
  t_generator.set_length(deja_vu_length);
  t_generator.set_pulse_width_mean(float(state.t_pulse_width_mean) / 256.0f);
  t_generator.set_pulse_width_std(float(state.t_pulse_width_std) / 256.0f);
  t_generator.Process(
      block->input_patched[0],
      &t_section_reset,
      t_clock,
      ramps,
      gates,
      size);

  // Generate voltages for X-section (40%).
  float note_cv_1 = cv_reader.channel(ADC_CHANNEL_X_SPREAD).scaled_raw_cv();
  float note_cv_2 = cv_reader.channel(ADC_CHANNEL_X_SPREAD_2).scaled_raw_cv();
  float note_cv = 0.5f * (note_cv_1 + note_cv_2);
  float u = note_filter.Process(0.5f * (note_cv + 1.0f));

  // V/Oct correction curve. Skipped while recording a scale: the recorded
  // voltages must not depend on whether Transpose happens to be enabled.
  if (state.x_register_mode == X_REGISTER_MODE_VOCT_OFFSET
      && !ui.recording_scale()) {
    static const float kVoctCorrectionTable[] = {
      0.015f,  // u=0.000 -5.0V
      0.136f,  // u=0.125 -3.75V
      0.257f,  // u=0.250 -2.5V
      0.378f,  // u=0.375 -1.25V
      0.499f,  // u=0.500  0.0V
      0.620f,  // u=0.625 +1.25V
      0.741f,  // u=0.750 +2.5V
      0.861f,  // u=0.875 +3.75V
      0.982f,  // u=1.000 +5.0V
    };
    // The 9-entry table spans [0, 1]; clamp so a CV at or beyond +5V can't
    // interpolate past the last entry (out-of-bounds flash read).
    CONSTRAIN(u, 0.0f, 0.999995f);
    u = Interpolate(kVoctCorrectionTable, u, 8.0f);
  }

  if (test_adc_noise) {
    static float note_lp = 0.0f;
    float note = note_cv_1;
    ONE_POLE(note_lp, note, 0.0001f);
    float cents = (note - note_lp) * 1200.0f * 5.0f;
    fill(&voltages[0], &voltages[4 * size], cents);
  } else if (ui.recording_scale()) {
    float voltage = (u - 0.5f) * 10.0f;
    for (size_t i = 0; i < size; ++i) {
      GateFlags gate = block->input_patched[1]
          ? block->input[1][i]
          : GATE_FLAG_LOW;
      if (gate & GATE_FLAG_RISING) {
        scale_recorder.NewNote(voltage);
      }
      if (gate & GATE_FLAG_HIGH) {
        scale_recorder.UpdateVoltage(voltage);
      }
      if (gate & GATE_FLAG_FALLING) {
        scale_recorder.AcceptNote();
      }
    }
    fill(&voltages[0], &voltages[4 * size], voltage);
  } else {
    x.control_mode = ControlMode(state.x_control_mode);
    x.voltage_range = VoltageRange(state.x_range % 3);
    x.envelope_retrigger = state.x_envelope_retrigger;
    x.acid_half_time = grids_mode;
    bool is_register = (state.x_register_mode == X_REGISTER_MODE_REGISTER);
    bool is_voct_offset = (state.x_register_mode == X_REGISTER_MODE_VOCT_OFFSET);
    // Register modes are meaningless in envelope mode (the X trio carries
    // envelopes, not pitches or shift-register data); ignore a leftover
    // setting so it can't halve the Spread CV response or shift outputs.
    if (state.x_control_mode == CONTROL_MODE_ENVELOPE) {
      is_register = is_voct_offset = false;
    }
    x.register_mode = is_register;
    // Transpose is a global offset applied at the DAC stage; the three X
    // outputs stay independent (shift-register replay is External only).
    x.use_shift_register = is_register;
    x.register_value = u;
    cv_reader.set_attenuverter(
        ADC_CHANNEL_X_SPREAD, (is_register || is_voct_offset) ? 0.5f : 1.0f);

    if (grids_mode && state.grids_knob_swap) {
      // Swapped: X params from stored alt fields + CV from jacks
      // CV goes to its normal destination (X params) unless rerouted by CV swap
      x.spread = static_cast<float>(state.x_spread_alt) / 255.0f;
      // In External mode the Spread jack carries register data, and in
      // Transpose mode the V/Oct offset — neither may leak into spread.
      if (!is_register && !is_voct_offset
          && settings.grids_chaos_cv_swap() != 1) {
        x.spread += cv_reader.channel(ADC_CHANNEL_X_SPREAD).cv();
        CONSTRAIN(x.spread, 0.0f, 1.0f);
      }

      x.bias = static_cast<float>(state.x_bias_alt) / 255.0f;
      if (settings.grids_y_cv_swap() != 1) {
        x.bias += cv_reader.channel(ADC_CHANNEL_X_BIAS).cv();
        CONSTRAIN(x.bias, 0.0f, 1.0f);
      }

      x.steps = static_cast<float>(state.x_steps_alt) / 255.0f;
      if (settings.grids_x_cv_swap() != 1) {
        x.steps += cv_reader.channel(ADC_CHANNEL_X_STEPS).cv();
        CONSTRAIN(x.steps, 0.0f, 1.0f);
      }
    } else {
      // Normal: X params from ADC (with CV swap isolation). Where the CV
      // jack is repurposed, read the stored pot (lock/catch-up aware), not
      // the live one, so shift-combo edits don't sweep the parameter.
      if (is_voct_offset || (grids_mode && settings.grids_chaos_cv_swap() == 1)) {
        x.spread = cv_reader.channel(ADC_CHANNEL_X_SPREAD).unscaled_stored_pot();
      } else {
        x.spread = parameters[ADC_CHANNEL_X_SPREAD];
      }

      if (grids_mode && settings.grids_y_cv_swap() == 1) {
        x.bias = cv_reader.channel(ADC_CHANNEL_X_BIAS).unscaled_stored_pot();
      } else {
        x.bias = parameters[ADC_CHANNEL_X_BIAS];
      }

      if (grids_mode && settings.grids_x_cv_swap() == 1) {
        x.steps = cv_reader.channel(ADC_CHANNEL_X_STEPS).unscaled_stored_pot();
      } else {
        x.steps = parameters[ADC_CHANNEL_X_STEPS];
      }
    }

    x.deja_vu = x_deja_vu_state == DEJA_VU_LOCKED
        ? 0.5f
        : (x_deja_vu_state == DEJA_VU_ON ? deja_vu : 0.0f);

    x.length = deja_vu_length;
    x.ratio.p = 1;
    x.ratio.q = 1;
  
    y.control_mode = CONTROL_MODE_IDENTICAL;
    y.voltage_range = VoltageRange(state.y_range);
    y.envelope_retrigger = 0;
    y.acid_half_time = false;
    y.register_mode = false;
    y.use_shift_register = false;
    y.register_value = 0.0f;
    y.spread = float(state.y_spread) / 256.0f;
    y.bias = float(state.y_bias) / 256.0f;
    y.steps = float(state.y_steps) / 256.0f;
    y.deja_vu = 0.0f;
    y.length = 1;
    y.ratio = y_divider_ratios[
        static_cast<uint16_t>(state.y_divider) * 12 >> 8];
    
    if (settings.dirty_scale_index() != -1) {
      int i = settings.dirty_scale_index();
      if (i < kNumScales) {
        xy_generator.LoadScale(i, settings.persistent_data().scale[i]);
      }
      settings.set_dirty_scale_index(-1);
    }
    
    // x_scale == kNumScales is the acid chromatic sentinel; clamp it to a
    // valid scale for the quantizer (acid bypasses it via x.chromatic).
    int scale_idx = state.x_scale >= kNumScales ? 0 : state.x_scale;
    y.scale_index = x.scale_index = scale_idx;
    x.chromatic = (state.x_scale >= kNumScales);
    x.acid_fill_active = ui.acid_fill_active();
    x.acid_fill_flavor = ui.acid_fill_flavor();
    
    bool x_reset_cv_available = !(grids_mode && settings.grids_x_cv_swap() == 1);
    bool x_section_reset = (settings.explicit_reset() & 2) && x_reset_cv_available &&
      hidden_gates[ADC_CHANNEL_X_STEPS] & GATE_FLAG_RISING;
    if (xy_clock_source != CLOCK_SOURCE_EXTERNAL && (settings.explicit_reset() & 2)) {
      x_section_reset |= t_section_reset;
    }

    // Phase-lock the acid half-time divider to the Grids drum grid.
    if (grids_mode) {
      xy_generator.set_acid_align_step(t_generator.grids_step());
    }

    xy_generator.Process(
        xy_clock_source,
        x,
        y,
        &x_section_reset,
        xy_clock,
        ramps,
        voltages,
        size);
  }
  
  const float* v = voltages;
  const bool* g = gates;

  float voct_offset = 0.0f;
  if (state.x_register_mode == X_REGISTER_MODE_VOCT_OFFSET
      && state.x_control_mode != CONTROL_MODE_ENVELOPE  // envelopes, not pitches
      && !test_adc_noise && !ui.recording_scale()) {
    voct_offset = (u - 0.5f) * 10.0f;
  }

  // In acid mode X2 carries gates and X3 an envelope/velocity signal;
  // transposition only makes sense on the X1 pitch output.
  float voct_offset_23 = voct_offset;
  if (state.x_control_mode == CONTROL_MODE_ACID
      && !test_adc_noise && !ui.recording_scale()) {
    voct_offset_23 = 0.0f;
  }

  for (size_t i = 0; i < size; ++i) {
    float val_x1 = *v++;
    float val_x2 = *v++;
    float val_x3 = *v++;
    float val_y  = *v++;

    block->cv_output[1][i] = DacCode(1, val_x1 + voct_offset); // X1
    block->cv_output[2][i] = DacCode(2, val_x2 + voct_offset_23); // X2
    block->cv_output[3][i] = DacCode(3, val_x3 + voct_offset_23); // X3
    
    if (grids_mode) {
      float accent_voltage = 0.0f;
      bool current_accent = accent_cv_delay_tail[0];
      uint8_t variation = state.grids_accent_variation;

      if (t_generator.get_and_clear_accent_triggered()) {
        if (variation < 120) {
          // Left side: random window
          float min_voltage = 5.0f * (static_cast<float>(variation) / 119.0f);
          held_accent_voltage = min_voltage + (5.0f - min_voltage) * t_generator.get_random_accent_voltage();
        } else if (variation > 136) {
          // Right side: dynamic velocity
          float knob_amount = static_cast<float>(variation - 137) / 118.0f;
          float min_voltage = 5.0f - 5.0f * knob_amount;
          float velocity = t_generator.get_accent_velocity();
          held_accent_voltage = min_voltage + (5.0f - min_voltage) * velocity;
        } else {
          held_accent_voltage = 5.0f;
        }
      }

      // Accent voltage hold logic
      bool variation_active = (variation < 120 || variation > 136);
      if (state.grids_accent_hang && variation_active) {
        accent_voltage = held_accent_voltage;  // always hold (S&H)
      } else if (current_accent) {
        accent_voltage = held_accent_voltage;
      }

      block->cv_output[0][i] = DacCode(0, accent_voltage);

      accent_cv_delay_tail[0] = accent_cv_delay_tail[1];
      accent_cv_delay_tail[1] = t_generator.get_accent_gate(i);
    } else {
      block->cv_output[0][i] = DacCode(0, val_y);
    }

    // T1 and T3 Gates
    bool gate_t1 = *g++;
    bool gate_t3 = *g++;

    if (grids_mode) {
      block->gate_output[0][i + kGateDelay] = gate_t1;
      block->gate_output[1][i + kGateDelay] = t_generator.get_hh_gate(i);
      block->gate_output[2][i + kGateDelay] = gate_t3;
    } else {
      block->gate_output[0][i + kGateDelay] = gate_t1;
      block->gate_output[1][i + kGateDelay] = (ramps.master[i] < 0.5f);
      block->gate_output[2][i + kGateDelay] = gate_t3;
    }
  }
  
  for (size_t i = 0; i < kNumGateOutputs; ++i) {
    for (size_t j = 0; j < kGateDelay; ++j) {
      block->gate_output[i][j] = gate_delay_tail[i][j];
      gate_delay_tail[i][j] = block->gate_output[i][size + j];
    }
  }

  if (PROFILE_RENDER) {
    TOC;
  }
}

void Init() {
  System sys;
  sys.Init(true);
  settings.Init();
  
  clock_inputs.Init();
  dac.Init(kSampleRate, 1);
  rng.Init();
  note_filter.Init();
  gate_outputs.Init();
  io_buffer.Init();
    
  deja_vu_length_quantizer.Init(
      sizeof(loop_length) / sizeof(int), 0.25f, false);
  cv_reader.Init(settings.mutable_calibration_data());
  scale_recorder.Init();
  ui.Init(&settings, &cv_reader, &scale_recorder, &clock_inputs);
  
  if (settings.freshly_baked()) {
    settings.ProgramOptionBytes();
    if (PROFILE_INTERRUPT || PROFILE_RENDER) {
      DebugPin::Init();
    } else {
      debug_port.Init();
    }
  }
  
  random_generator.Init(1);
  random_stream.Init(&random_generator);
  t_generator.Init(&random_stream, static_cast<float>(kSampleRate));
  xy_generator.Init(&random_stream, static_cast<float>(kSampleRate));

  for (size_t i = 0; i < kNumScales; ++i) {
    xy_generator.LoadScale(i, settings.persistent_data().scale[i]);
  }
  
  for (size_t i = 0; i < kNumGateOutputs; ++i) {
    self_patching_detector[i].Init(i);
  }
  
  sys.StartTimers();
  dac.Start(&FillBuffer);
}

int main(void) {
  Init();
  while (1) {
    ui.DoEvents();
    io_buffer.Process(ui.output_test_mode() ? &ProcessTest : &Process);
  }
}
