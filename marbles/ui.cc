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
// User interface.

#include "marbles/ui.h"
#include "marbles/random/t_generator.h"

#include <algorithm>

#include "stmlib/system/system_clock.h"

#include "marbles/drivers/clock_inputs.h"
#include "marbles/cv_reader.h"
#include "marbles/scale_recorder.h"

namespace marbles {

const int32_t kLongPressDuration = 2000;
const int32_t kMediumPressDuration = 1000;

using namespace std;
using namespace stmlib;

/* static */
const LedColor Ui::palette_[4] = {
  LED_COLOR_GREEN,
  LED_COLOR_YELLOW,
  LED_COLOR_RED,
  LED_COLOR_OFF
};

/* static */
AlternateKnobMapping Ui::alternate_knob_mappings_[ADC_CHANNEL_LAST];
static uint8_t prev_t_model = 255;
static int8_t grids_held_first = -1;

void Ui::Init(
    Settings* settings,
    CvReader* cv_reader,
    ScaleRecorder* scale_recorder,
    ClockInputs* clock_inputs) {
  settings_ = settings;
  cv_reader_ = cv_reader;
  scale_recorder_ = scale_recorder;
  clock_inputs_ = clock_inputs;
  
  leds_.Init();
  switches_.Init();
  queue_.Init();
  
  // Initialize generator from settings_->state();
  fill(&pot_value_[0], &pot_value_[ADC_CHANNEL_LAST], 0.0f);
  
  State* state = settings_->mutable_state();
  alternate_knob_mappings_[ADC_CHANNEL_T_BIAS].unlock_switch = SWITCH_T_MODEL;
  alternate_knob_mappings_[ADC_CHANNEL_T_BIAS].destination = &state->t_pulse_width_mean;

  alternate_knob_mappings_[ADC_CHANNEL_T_JITTER].unlock_switch = SWITCH_T_MODEL;
  alternate_knob_mappings_[ADC_CHANNEL_T_JITTER].destination = &state->t_pulse_width_std;

  alternate_knob_mappings_[ADC_CHANNEL_T_RATE].unlock_switch = SWITCH_X_MODE;
  alternate_knob_mappings_[ADC_CHANNEL_T_RATE].destination = &state->y_divider;

  alternate_knob_mappings_[ADC_CHANNEL_X_SPREAD].unlock_switch = SWITCH_X_MODE;
  alternate_knob_mappings_[ADC_CHANNEL_X_SPREAD].destination = &state->y_spread;

  alternate_knob_mappings_[ADC_CHANNEL_X_BIAS].unlock_switch = SWITCH_X_MODE;
  alternate_knob_mappings_[ADC_CHANNEL_X_BIAS].destination = &state->y_bias;

  alternate_knob_mappings_[ADC_CHANNEL_X_STEPS].unlock_switch = SWITCH_X_MODE;
  alternate_knob_mappings_[ADC_CHANNEL_X_STEPS].destination = &state->y_steps;

  // These are only used in Grids mode
  alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_AMOUNT].unlock_switch = SWITCH_X_MODE;
  alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_AMOUNT].destination = NULL; 
  alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_LENGTH].unlock_switch = SWITCH_X_MODE;
  alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_LENGTH].destination = NULL; 

  setting_modification_flag_ = false;
  grids_save_flag_ = false;
  output_test_mode_ = false;
  
  if (switches_.pressed_immediate(SWITCH_X_MODE)) {
    if (state->color_blind == 1) {
      state->color_blind = 0;
    } else {
      state->color_blind = 1; 
    }
    settings_->SaveState();
  }
  
  if (switches_.pressed_immediate(SWITCH_X_DEJA_VU)) {
    settings_->ProgramOptionBytes();
  }
  
  deja_vu_lock_ = false;
}

void Ui::SaveState() {
  settings_->SaveState();
}

void Ui::Poll() {
  // 1kHz.
  system_clock.Tick();
  switches_.Debounce();

  State* s = settings_->mutable_state();

  if (s->t_model == marbles::T_GENERATOR_MODEL_GRIDS && prev_t_model != marbles::T_GENERATOR_MODEL_GRIDS) {
    // Just entered Grids mode (not on boot) - capture current rate
    if (prev_t_model != 255) {
      uint8_t rate_knob = static_cast<uint8_t>(cv_reader_->channel(ADC_CHANNEL_T_RATE).unscaled_pot() * 255.0f);
      s->t_rate_stored = rate_knob;
      s->grids_hh_density = rate_knob;
    }
    grids_held_first = -1;
  }
  prev_t_model = s->t_model;
  if (s->t_model < marbles::T_GENERATOR_MODEL_GRIDS) {
    grids_held_first = -1;
  }

  if (s->t_model == marbles::T_GENERATOR_MODEL_GRIDS) {
    // GRIDS MODE: Remap Shift-Knobs to Grids Variables
    alternate_knob_mappings_[ADC_CHANNEL_X_STEPS].destination = &s->x_steps_alt;
    alternate_knob_mappings_[ADC_CHANNEL_X_BIAS].destination  = &s->x_bias_alt;
    alternate_knob_mappings_[ADC_CHANNEL_X_SPREAD].destination = &s->x_spread_alt;
    alternate_knob_mappings_[ADC_CHANNEL_T_RATE].destination   = &s->t_rate_stored;
    alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_AMOUNT].destination = &s->grids_accent_threshold;
    alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_LENGTH].destination = &s->grids_accent_variation;
    // swing is done in Ui::UpdateHiddenParameters()
  } else {
    // STANDARD MODE: Restore Original Mappings
    alternate_knob_mappings_[ADC_CHANNEL_X_STEPS].destination = &s->y_steps;
    alternate_knob_mappings_[ADC_CHANNEL_X_BIAS].destination  = &s->y_bias;
    alternate_knob_mappings_[ADC_CHANNEL_X_SPREAD].destination = &s->y_spread;
    alternate_knob_mappings_[ADC_CHANNEL_T_RATE].destination = &s->y_divider;
    alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_AMOUNT].destination = NULL;
    alternate_knob_mappings_[ADC_CHANNEL_DEJA_VU_LENGTH].destination = NULL;
  }
  
  for (int i = 0; i < SWITCH_LAST; ++i) {
    if (switches_.just_pressed(Switch(i))) {
      queue_.AddEvent(CONTROL_SWITCH, i, 0);
      press_time_[i] = system_clock.milliseconds();
      ignore_release_[i] = false;
      if (settings_->state().t_model >= T_GENERATOR_MODEL_GRIDS && grids_held_first == -1) {
        if (i == SWITCH_T_MODEL || i == SWITCH_X_MODE) {
          grids_held_first = i;
        }
      }
    }
    if (switches_.pressed(Switch(i)) && !ignore_release_[i]) {
      int32_t pressed_time = system_clock.milliseconds() - press_time_[i];
      if (pressed_time > kLongPressDuration && !setting_modification_flag_) {
        bool suppress = (i == SWITCH_T_MODEL &&
            settings_->state().t_model >= T_GENERATOR_MODEL_GRIDS);
        if (!suppress) {
          queue_.AddEvent(CONTROL_SWITCH, i, pressed_time);
          ignore_release_[i] = true;
        }
      }
    }
    if (switches_.released(Switch(i)) && !ignore_release_[i]) {
      queue_.AddEvent(
          CONTROL_SWITCH,
          i,
          system_clock.milliseconds() - press_time_[i] + 1);
      ignore_release_[i] = true;
    }
  }

  if (switches_.released(SWITCH_T_MODEL)) {
    if (grids_held_first == SWITCH_T_MODEL) {
      grids_held_first = switches_.pressed(SWITCH_X_MODE) ? SWITCH_X_MODE : -1;
    }
  }
  if (switches_.released(SWITCH_X_MODE)) {
    if (grids_held_first == SWITCH_X_MODE) {
      grids_held_first = switches_.pressed(SWITCH_T_MODEL) ? SWITCH_T_MODEL : -1;
    }
  }

  // Flush all pending state changes once both shift buttons are released
  if (grids_save_flag_
      && !switches_.pressed(SWITCH_X_MODE)
      && !switches_.pressed(SWITCH_T_MODEL)) {
    grids_save_flag_ = false;
    SaveState();
  }

  UpdateLEDs();
}

/* static */
LedColor Ui::MakeColor(uint8_t value, bool color_blind) {
  bool slow_blink = (system_clock.milliseconds() & 255) > 128;

  uint8_t bank = value >= 3 ? 1 : 0;
  value -= bank * 3;
  
  LedColor color = palette_[value];
  if (color_blind) {
    uint8_t pwm_counter = system_clock.milliseconds() & 15;
    uint8_t triangle = (system_clock.milliseconds() >> 5) & 31;
    triangle = triangle < 16 ? triangle : 31 - triangle;
  
    if (value == 0) {
      color = pwm_counter < (4 + (triangle >> 2))
          ? LED_COLOR_GREEN
          : LED_COLOR_OFF;
    } else if (value == 1) {
      color = LED_COLOR_YELLOW;
    } else {
      color = pwm_counter == 0 ? LED_COLOR_RED : LED_COLOR_OFF;
    }
  }

  return slow_blink || !bank ? color : LED_COLOR_OFF;
}

/* static */
LedColor Ui::DejaVuColor(DejaVuState state, bool lock) {
  if (state == DEJA_VU_OFF) {
    return LED_COLOR_OFF;
  } else if (state == DEJA_VU_ON) {
    if (lock) {
      int slow_triangle = (system_clock.milliseconds() & 1023) >> 5;
      slow_triangle = slow_triangle >= 16 ? 31 - slow_triangle : slow_triangle;
      int pw = system_clock.milliseconds() & 15;
      return slow_triangle >= pw ? LED_COLOR_GREEN : LED_COLOR_OFF;
    } else {
      return LED_COLOR_GREEN;
    }
  } else {
    int fast_triangle = (system_clock.milliseconds() & 511) >> 4;
    fast_triangle = fast_triangle >= 16 ? 31 - fast_triangle : fast_triangle;
    int pw = system_clock.milliseconds() & 15;

    return fast_triangle >= pw ? LED_COLOR_GREEN : LED_COLOR_OFF;
  }
}

void Ui::UpdateLEDs() {
  bool blink = (system_clock.milliseconds() & 127) > 64;
  bool slow_blink = (system_clock.milliseconds() & 255) > 128;
  bool fast_blink = (system_clock.milliseconds() & 63) > 32;
  const State& state = settings_->state();
  bool cb = state.color_blind == 1;
  
  LedColor scale_color = state.x_scale < 3
      ? (slow_blink ? palette_[state.x_scale] : LED_COLOR_OFF)
      : (fast_blink ? palette_[state.x_scale - 3] : LED_COLOR_OFF);
  
  if (cb) {
    int poly_counter = (system_clock.milliseconds() >> 6) % 12;
    if ((poly_counter >> 1) < (state.x_scale + 1) && (poly_counter & 1)) {
      scale_color = LED_COLOR_YELLOW;
    } else {
      scale_color = LED_COLOR_OFF;
    }
  }
  
  leds_.Clear();
  
  switch (mode_) {
    case UI_MODE_NORMAL:
    case UI_MODE_RECORD_SCALE:
      {
        // Grids advanced settings display: T_MODEL held
        int32_t t_model_held = system_clock.milliseconds() - press_time_[SWITCH_T_MODEL];
        bool t_model_exiting = t_model_held >= kLongPressDuration && !ignore_release_[SWITCH_T_MODEL];
        if (settings_->state().t_model == T_GENERATOR_MODEL_GRIDS &&
            switches_.pressed(SWITCH_T_MODEL) && grids_held_first == SWITCH_T_MODEL &&
            !t_model_exiting) {
          leds_.set(LED_T_RANGE, state.grids_henri ? LED_COLOR_GREEN : LED_COLOR_OFF);
          leds_.set(LED_X_EXT, state.grids_accent_hang ? LED_COLOR_GREEN : LED_COLOR_OFF);

          leds_.set(LED_T_DEJA_VU, state.grids_sync_playheads ? LED_COLOR_GREEN : LED_COLOR_OFF);
          leds_.set(LED_X_DEJA_VU, state.grids_loop_start_at_one ? LED_COLOR_GREEN : LED_COLOR_OFF);

          leds_.set(LED_X_CONTROL_MODE, state.explicit_reset ? LED_COLOR_YELLOW : LED_COLOR_OFF);
          leds_.set(LED_X_RANGE, state.grids_knob_swap ? LED_COLOR_GREEN : LED_COLOR_OFF);
        } else if (settings_->state().t_model == T_GENERATOR_MODEL_GRIDS &&
            switches_.pressed(SWITCH_X_MODE) && grids_held_first == SWITCH_X_MODE) {

          bool fast_blink = (system_clock.milliseconds() & 127) > 64;

          // Map X: off=off, 1=green (steps), 2=yellow (t_bias)
          LedColor x_color = LED_COLOR_OFF;
          if (state.grids_x_cv_swap == 1) x_color = LED_COLOR_GREEN;
          else if (state.grids_x_cv_swap == 2) x_color = fast_blink ? LED_COLOR_GREEN : LED_COLOR_OFF;
          leds_.set(LED_T_RANGE, x_color);

          // Chaos: off=off, 1=green (spread), 2=yellow (rate)
          LedColor chaos_color = LED_COLOR_OFF;
          if (state.grids_chaos_cv_swap == 1) chaos_color = LED_COLOR_GREEN;
          else if (state.grids_chaos_cv_swap == 2) chaos_color = fast_blink ? LED_COLOR_GREEN : LED_COLOR_OFF;
          leds_.set(LED_X_EXT, chaos_color);

          // Map Y: off=off, 1=green (x_bias), 2=yellow (jitter)
          LedColor y_color = LED_COLOR_OFF;
          if (state.grids_y_cv_swap == 1) y_color = LED_COLOR_GREEN;
          else if (state.grids_y_cv_swap == 2) y_color = fast_blink ? LED_COLOR_GREEN : LED_COLOR_OFF;
          leds_.set(LED_X_RANGE, y_color);

          // T mode LED: bank color + interpolation blink
          {
            LedColor bank_colors[] = { LED_COLOR_GREEN, LED_COLOR_YELLOW, LED_COLOR_RED };
            LedColor bank_color = bank_colors[state.grids_bank];
            bool interp_blink = !state.grids_interpolation && slow_blink;
            leds_.set(LED_T_MODEL, state.grids_interpolation ? bank_color
                : (interp_blink ? bank_color : LED_COLOR_OFF));
          }

          // T deja vu lock CV swap
          LedColor t_dv_color = LED_COLOR_OFF;
          if (state.deja_vu_t_cv_swap) t_dv_color = LED_COLOR_GREEN;
          leds_.set(LED_T_DEJA_VU, t_dv_color);

          // X deja vu lock CV swap
          LedColor x_dv_color = LED_COLOR_OFF;
          if (state.deja_vu_x_cv_swap) x_dv_color = LED_COLOR_GREEN;
          leds_.set(LED_X_DEJA_VU, x_dv_color);

        } else {
          leds_.set(LED_T_RANGE, MakeColor(state.t_range, cb));
          if (mode_ == UI_MODE_NORMAL) {
            if (state.x_control_mode == 5) {  // CONTROL_MODE_CHORD
              if (state.x_scale >= 6) {
                leds_.set(LED_X_RANGE, LED_COLOR_OFF);  // chromatic
              } else {
                leds_.set(LED_X_RANGE, scale_color);
              }
            } else {
              leds_.set(LED_X_RANGE,
                        state.x_register_mode == X_REGISTER_MODE_REGISTER
                            ? LED_COLOR_OFF
                            : MakeColor(state.x_range, cb));
            }
            if (state.x_control_mode == 5) {  // CONTROL_MODE_CHORD only (inversion via x_range)
              int inv = state.x_range;
              if (inv == 0) {
                leds_.set(LED_X_EXT, LED_COLOR_OFF);
              } else {
                uint32_t phase = system_clock.milliseconds() % 1000;
                bool led_on = false;
                for (int b = 0; b < inv; b++) {
                  uint32_t start = b * 200;
                  if (phase >= start && phase < start + 100) led_on = true;
                }
                leds_.set(LED_X_EXT, led_on ? LED_COLOR_GREEN : LED_COLOR_OFF);
              }
            } else {
              LedColor x_ext_color = LED_COLOR_OFF;
              if (state.x_register_mode == X_REGISTER_MODE_REGISTER) {
                x_ext_color = LED_COLOR_GREEN;
              } else if (state.x_register_mode == X_REGISTER_MODE_VOCT_OFFSET) {
                x_ext_color = slow_blink ? LED_COLOR_GREEN : LED_COLOR_OFF;
              }
              leds_.set(LED_X_EXT, x_ext_color);
            }
          } else {
            leds_.set(LED_X_RANGE, scale_color);
            leds_.set(LED_X_EXT, LED_COLOR_GREEN);
          }

          leds_.set(LED_T_MODEL, MakeColor(state.t_model, cb));
        
          leds_.set(LED_X_CONTROL_MODE, MakeColor(state.x_control_mode, cb));

          DejaVuState t_dv_display = DejaVuState(state.t_deja_vu);
          if (settings_->state().t_model == T_GENERATOR_MODEL_GRIDS && 
              state.deja_vu_t_cv_swap) {
            float gate_cv = cv_reader_->channel(ADC_CHANNEL_DEJA_VU_AMOUNT).scaled_raw_cv();
            bool gate_high = (gate_cv > 0.5f);
            if (gate_high) {
                if (state.t_deja_vu == DEJA_VU_ON || state.t_deja_vu == DEJA_VU_LOCKED) {
                    t_dv_display = DEJA_VU_OFF;
                } else {
                    t_dv_display = DEJA_VU_ON;
                }
            }
          }
          leds_.set(LED_T_DEJA_VU, DejaVuColor(t_dv_display, deja_vu_lock_)); 

          DejaVuState x_dv_display = DejaVuState(state.x_deja_vu);
          if (settings_->state().t_model == T_GENERATOR_MODEL_GRIDS && 
              state.deja_vu_x_cv_swap) {
              float gate_cv = cv_reader_->channel(ADC_CHANNEL_DEJA_VU_AMOUNT).scaled_raw_cv();
              bool gate_high = (gate_cv > 0.5f);
              if (gate_high) {
                if (state.x_deja_vu == DEJA_VU_ON || state.x_deja_vu == DEJA_VU_LOCKED) {
                    x_dv_display = DEJA_VU_OFF;
                } else {
                    x_dv_display = DEJA_VU_ON;
                }
            }
          }
          leds_.set(LED_X_DEJA_VU, DejaVuColor(x_dv_display, deja_vu_lock_));
        }
      }
      break;

    case UI_MODE_SELECT_SCALE:
      leds_.set(LED_X_RANGE, scale_color);
      break;
    
    case UI_MODE_CALIBRATION_1:
      leds_.set(LED_T_RANGE, blink ? MakeColor(0, cb) : LED_COLOR_OFF);
      break;

    case UI_MODE_CALIBRATION_2:
      leds_.set(LED_T_RANGE, blink ? MakeColor(1, cb) : LED_COLOR_OFF);
      break;

    case UI_MODE_CALIBRATION_3:
      leds_.set(LED_X_RANGE, blink ? MakeColor(0, cb) : LED_COLOR_OFF);
      break;

    case UI_MODE_CALIBRATION_4:
      leds_.set(LED_X_RANGE, blink ? MakeColor(1, cb) : LED_COLOR_OFF);
      break;
    
    case UI_MODE_PANIC:
      leds_.set(LED_T_MODEL, blink ? LED_COLOR_RED : LED_COLOR_OFF);
      leds_.set(LED_T_RANGE, !blink ? LED_COLOR_RED : LED_COLOR_OFF);
      leds_.set(LED_X_CONTROL_MODE, !blink ? LED_COLOR_RED : LED_COLOR_OFF);
      leds_.set(LED_X_RANGE, blink ? LED_COLOR_RED : LED_COLOR_OFF);
      break;
      
    case UI_MODE_DISPLAY_RESET_MODE:
      {
        const bool l = blink && state.explicit_reset;
        leds_.set(LED_T_MODEL, l ? LED_COLOR_YELLOW : LED_COLOR_OFF);
        leds_.set(LED_X_CONTROL_MODE, l ? LED_COLOR_YELLOW : LED_COLOR_OFF);
      }
      break;
  }
  leds_.Write();
}

void Ui::FlushEvents() {
  queue_.Flush();
}

void Ui::OnSwitchPressed(const Event& e) {

}

void Ui::OnSwitchReleased(const Event& e) {
  if (setting_modification_flag_) {
    for (int i = 0; i < ADC_CHANNEL_LAST; ++i) {
      cv_reader_->mutable_channel(i)->UnlockPot();
    }
    setting_modification_flag_ = false;
    return;
  }
  
  // Check if the other switch is still pressed.
  if (e.control_id == SWITCH_T_RANGE && switches_.pressed(SWITCH_X_RANGE)) {
    mode_ = UI_MODE_CALIBRATION_1;
    ignore_release_[SWITCH_T_RANGE] = ignore_release_[SWITCH_X_RANGE] = true;
    return;
  }
  
  State* state = settings_->mutable_state();

  if (state->t_model == T_GENERATOR_MODEL_GRIDS) {
    if ((e.control_id == SWITCH_T_RANGE && switches_.pressed(SWITCH_X_MODE)) ||
        (e.control_id == SWITCH_X_MODE && switches_.pressed(SWITCH_T_RANGE))) {
      ignore_release_[SWITCH_X_MODE] = ignore_release_[SWITCH_T_RANGE] = true;
      state->grids_x_cv_swap = (state->grids_x_cv_swap + 1) % 3;
      grids_save_flag_ = true;
      return;
    }

    if ((e.control_id == SWITCH_X_EXT && switches_.pressed(SWITCH_X_MODE)) ||
        (e.control_id == SWITCH_X_MODE && switches_.pressed(SWITCH_X_EXT))) {
      ignore_release_[SWITCH_X_MODE] = ignore_release_[SWITCH_X_EXT] = true;
      state->grids_chaos_cv_swap = (state->grids_chaos_cv_swap + 1) % 3;
      grids_save_flag_ = true;
      return;
    }

    if ((e.control_id == SWITCH_X_RANGE && switches_.pressed(SWITCH_X_MODE)) ||
        (e.control_id == SWITCH_X_MODE && switches_.pressed(SWITCH_X_RANGE))) {
      ignore_release_[SWITCH_X_MODE] = ignore_release_[SWITCH_X_RANGE] = true;
      state->grids_y_cv_swap = (state->grids_y_cv_swap + 1) % 3;
      grids_save_flag_ = true;
      return;
    }

    // T side deja vu lock CV swap
    if ((e.control_id == SWITCH_T_DEJA_VU && switches_.pressed(SWITCH_X_MODE)) ||
        (e.control_id == SWITCH_X_MODE && switches_.pressed(SWITCH_T_DEJA_VU))) {
        ignore_release_[SWITCH_X_MODE] = ignore_release_[SWITCH_T_DEJA_VU] = true;
        state->deja_vu_t_cv_swap = !state->deja_vu_t_cv_swap;
        grids_save_flag_ = true;
        return;
    }

    // X side deja vu lock CV swap
    if ((e.control_id == SWITCH_X_DEJA_VU && switches_.pressed(SWITCH_X_MODE)) ||
        (e.control_id == SWITCH_X_MODE && switches_.pressed(SWITCH_X_DEJA_VU))) {
        ignore_release_[SWITCH_X_MODE] = ignore_release_[SWITCH_X_DEJA_VU] = true;
        state->deja_vu_x_cv_swap = !state->deja_vu_x_cv_swap;
        grids_save_flag_ = true;
        return;
    }
  }

  // T pressed while X held > Cycle bank
  if (e.control_id == SWITCH_T_MODEL && switches_.pressed(SWITCH_X_MODE)) {
    bool is_grids = state->t_model >= T_GENERATOR_MODEL_GRIDS;
    if (!is_grids || grids_held_first == SWITCH_X_MODE) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_X_MODE] = true;
      uint8_t combined = state->grids_bank + (state->grids_interpolation ? 0 : 3);
      combined = (combined + 1) % 6;
      state->grids_bank = combined % 3;
      state->grids_interpolation = (combined < 3) ? 1 : 0;
      grids_save_flag_ = true;
      return;
    }
  }

  // X pressed while T held > Toggle explicit reset
  if (e.control_id == SWITCH_X_MODE && switches_.pressed(SWITCH_T_MODEL)) {
    bool is_grids = state->t_model >= T_GENERATOR_MODEL_GRIDS;
    if (!is_grids || grids_held_first == SWITCH_T_MODEL) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_X_MODE] = true;
      state->explicit_reset = !state->explicit_reset;
      if (!is_grids) {
        mode_ = UI_MODE_DISPLAY_RESET_MODE;
        SaveState();
      } else {
        grids_save_flag_ = true;
      }
      return;
    }
  }

  // Grids advanced settings: T_MODEL held + button tap
  if (state->t_model == T_GENERATOR_MODEL_GRIDS && switches_.pressed(SWITCH_T_MODEL) && grids_held_first == SWITCH_T_MODEL) {
    if (e.control_id == SWITCH_T_RANGE) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_T_RANGE] = true;
      state->grids_henri = !state->grids_henri;
      grids_save_flag_ = true;
      return;
    }
    if (e.control_id == SWITCH_X_EXT) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_X_EXT] = true;
      state->grids_accent_hang = !state->grids_accent_hang;
      grids_save_flag_ = true;
      return;
    }
    if (e.control_id == SWITCH_T_DEJA_VU) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_T_DEJA_VU] = true;
      state->grids_sync_playheads = !state->grids_sync_playheads;
      grids_save_flag_ = true;
      return;
    }
    if (e.control_id == SWITCH_X_DEJA_VU) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_X_DEJA_VU] = true;
      state->grids_loop_start_at_one = !state->grids_loop_start_at_one;
      grids_save_flag_ = true;
      return;
    }
    if (e.control_id == SWITCH_X_MODE) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_X_MODE] = true;
      state->explicit_reset = !state->explicit_reset;
      grids_save_flag_ = true;
      return;
    }
    if (e.control_id == SWITCH_X_RANGE) {
      ignore_release_[SWITCH_T_MODEL] = ignore_release_[SWITCH_X_RANGE] = true;
      // Capture current knob positions into alt fields before swapping
      // (the fields switch meaning, so initialize with current ADC values)
      state->x_steps_alt = static_cast<uint8_t>(
          cv_reader_->channel(ADC_CHANNEL_X_STEPS).unscaled_pot() * 255.0f);
      state->x_bias_alt = static_cast<uint8_t>(
          cv_reader_->channel(ADC_CHANNEL_X_BIAS).unscaled_pot() * 255.0f);
      state->x_spread_alt = static_cast<uint8_t>(
          cv_reader_->channel(ADC_CHANNEL_X_SPREAD).unscaled_pot() * 255.0f);
      state->grids_knob_swap = !state->grids_knob_swap;
      grids_save_flag_ = true;
      return;
    }
  }

  switch (e.control_id) {
    case SWITCH_T_DEJA_VU:
        if (state->t_deja_vu == DEJA_VU_OFF) {
          state->t_deja_vu = e.data >= kMediumPressDuration
              ? DEJA_VU_LOCKED
              : DEJA_VU_ON;
        } else if (state->t_deja_vu == DEJA_VU_LOCKED) {
          state->t_deja_vu = DEJA_VU_ON;
        } else {
          state->t_deja_vu = e.data >= kMediumPressDuration
              ? DEJA_VU_LOCKED
              : DEJA_VU_OFF;
        }
      break;

    case SWITCH_X_DEJA_VU:
      if (state->x_deja_vu == DEJA_VU_OFF) {
        state->x_deja_vu = e.data >= kMediumPressDuration
            ? DEJA_VU_LOCKED
            : DEJA_VU_ON;
      } else if (state->x_deja_vu == DEJA_VU_LOCKED) {
        state->x_deja_vu = DEJA_VU_ON;
      } else {
        state->x_deja_vu = e.data >= kMediumPressDuration
            ? DEJA_VU_LOCKED
            : DEJA_VU_OFF;
      }
      break;
    
    case SWITCH_T_MODEL:
      {
        uint8_t bank = state->t_model / 3;
        if (bank) {
          // In Grids mode: only long press (>=2s) exits
          if (e.data >= kLongPressDuration) {
            state->t_model -= 3;
          }
        } else {
          if (e.data >= kMediumPressDuration) {
            state->t_model += 3;
          } else {
            state->t_model = (state->t_model + 1) % 3;
          }
        }
        SaveState();
      }
      break;

    case SWITCH_T_RANGE:
      {
        if (mode_ >= UI_MODE_CALIBRATION_1 && mode_ <= UI_MODE_CALIBRATION_4) {
          NextCalibrationStep();
        } else {
          state->t_range = (state->t_range + 1) % 3;
        }
        SaveState();
      }
      break;
    
    case SWITCH_X_MODE:
      // In Grids mode, only cycle on short tap
      if (state->t_model != T_GENERATOR_MODEL_GRIDS || e.data < 275) {
        state->x_control_mode = (state->x_control_mode + 1) % 6;
        if (state->x_control_mode != 4 && state->x_control_mode != 5 && state->x_scale > 5) {
          state->x_scale = 5;  // clamp chromatic back to last preset
        }
        SaveState();
      }
      break;
      
    case SWITCH_X_EXT:
      if (mode_ == UI_MODE_RECORD_SCALE) {
        int scale_index = settings_->state().x_scale;
        bool success = true;
        if (e.data >= kLongPressDuration) {
          settings_->ResetScale(scale_index);
        } else {
          success = scale_recorder_->ExtractScale(
              settings_->mutable_scale(scale_index));
        }
        if (success) {
          settings_->SavePersistentData();
          settings_->set_dirty_scale_index(scale_index);
        }
        mode_ = UI_MODE_NORMAL;
      } else if (e.data >= kLongPressDuration) {
        mode_ = UI_MODE_RECORD_SCALE;
        scale_recorder_->Clear();
      } else {
        if (state->x_control_mode == 5) {  // CONTROL_MODE_CHORD only
          state->x_range = (state->x_range + 1) % 4;  // inversion 0-3
        } else {
          state->x_register_mode = (state->x_register_mode + 1) % X_REGISTER_MODE_LAST;
        }
        SaveState();
      }
      break;

    case SWITCH_X_RANGE:
      if (mode_ >= UI_MODE_CALIBRATION_1 && mode_ <= UI_MODE_CALIBRATION_4) {
        NextCalibrationStep();
      } else if (state->x_control_mode == 5) {  // CONTROL_MODE_CHORD
        if (e.data < kLongPressDuration) {
          state->x_scale = (state->x_scale + 1) % 7;  // 0-5 scales, 6 chromatic
        }
      } else if (e.data >= kLongPressDuration) {
        if (mode_ == UI_MODE_NORMAL) {
          mode_ = UI_MODE_SELECT_SCALE;
        }
      } else if (mode_ == UI_MODE_SELECT_SCALE) {
        state->x_scale = (state->x_scale + 1) % kNumScales;
      } else {
        if (state->x_register_mode != X_REGISTER_MODE_REGISTER) {
          state->x_range = (state->x_range + 1) % 3;
        }
      }
      SaveState();
      break;
  }
}

void Ui::TerminateScaleRecording() {
  for (int i = 0; i < ADC_CHANNEL_LAST; ++i) {
    cv_reader_->mutable_channel(i)->UnlockPot();
  }
  mode_ = UI_MODE_NORMAL;
}

void Ui::NextCalibrationStep() {
  switch (mode_) {
    case UI_MODE_CALIBRATION_1:
      cv_reader_->CalibrateOffsets();
      cv_reader_->CalibrateRateC1();
      mode_ = UI_MODE_CALIBRATION_2;
      break;

    case UI_MODE_CALIBRATION_2:
      cv_reader_->CalibrateRateC3();
      mode_ = UI_MODE_CALIBRATION_3;
      break;

    case UI_MODE_CALIBRATION_3:
      cv_reader_->CalibrateSpreadC1();
      mode_ = UI_MODE_CALIBRATION_4;
      break;
    
    case UI_MODE_CALIBRATION_4:
      if (cv_reader_->CalibrateSpreadC3()) {
        settings_->SavePersistentData();
        mode_ = UI_MODE_NORMAL;
      } else {
        mode_ = UI_MODE_PANIC;
      }
      break;
      
    default:
      break;
  }
}

void Ui::UpdateHiddenParameters() {
  State* state = settings_->mutable_state();

  // Check if some pots have been moved.
  for (int i = 0; i < ADC_CHANNEL_LAST; ++i) {
    float new_value = cv_reader_->channel(i).unscaled_pot();
    float old_value = pot_value_[i];
    bool changed = fabs(new_value - old_value) >= 0.008f;
    if (changed) {
      pot_value_[i] = new_value;

      if (i == ADC_CHANNEL_T_JITTER && state->t_model == T_GENERATOR_MODEL_GRIDS) {
        if (switches_.pressed(SWITCH_X_MODE)) {
          // X Mode + Jitter = swing
          state->grids_swing = static_cast<uint8_t>(new_value * 255.0f);
          cv_reader_->mutable_channel(i)->LockPot();
          setting_modification_flag_ = true;
          grids_save_flag_ = true;
          continue;
        } else if (switches_.pressed(SWITCH_T_MODEL)) {
          // T Model + Jitter = pulse width
          state->t_pulse_width_std = static_cast<uint8_t>(new_value * 255.0f);
          cv_reader_->mutable_channel(i)->LockPot();
          setting_modification_flag_ = true;
          grids_save_flag_ = true;
          continue;
        }
      }

      if (i == ADC_CHANNEL_T_BIAS && state->t_model == T_GENERATOR_MODEL_GRIDS) {
        if (switches_.pressed(SWITCH_X_MODE)) {
          // X Mode + Bias = Offset
          state->grids_groove_offset = static_cast<uint8_t>(new_value * 255.0f);
          cv_reader_->mutable_channel(i)->LockPot();
          setting_modification_flag_ = true;
          grids_save_flag_ = true;
          continue;
        } else if (switches_.pressed(SWITCH_T_MODEL)) {
          // T Model + Bias = pulse width mean
          state->t_pulse_width_mean = static_cast<uint8_t>(new_value * 255.0f);
          cv_reader_->mutable_channel(i)->LockPot();
          setting_modification_flag_ = true;
          grids_save_flag_ = true;
          continue;
        }
      }

      // T_RATE in grids mode: normal = HH density, shift (X_MODE) = tempo
      if (i == ADC_CHANNEL_T_RATE && state->t_model == T_GENERATOR_MODEL_GRIDS) {
        if (!switches_.pressed(SWITCH_X_MODE)) {
          // Normal mode: update HH density
          state->grids_hh_density = static_cast<uint8_t>(new_value * 255.0f);
          continue;
        }
      }
      
      AlternateKnobMapping mapping = alternate_knob_mappings_[i];
      if (mapping.destination && switches_.pressed(mapping.unlock_switch)) {
        if (mapping.unlock_switch == SWITCH_T_RANGE && new_value < 0.1f) {
          new_value = 0.0f;
        }
        *mapping.destination = static_cast<uint8_t>(new_value * 255.0f);
        cv_reader_->mutable_channel(i)->LockPot();

        // The next time a switch is released, we unlock the pots.
        setting_modification_flag_ = true;
        grids_save_flag_ = true;
      }
    }
  }
}

void Ui::DoEvents() {
  while (queue_.available()) {
    Event e = queue_.PullEvent();
    if (e.control_type == CONTROL_SWITCH) {
      if (e.data == 0) {
        OnSwitchPressed(e);
      } else {
        OnSwitchReleased(e);
      }
    }
  }
  
  UpdateHiddenParameters();
  
  if (queue_.idle_time() > 800 && mode_ == UI_MODE_PANIC) {
    mode_ = UI_MODE_NORMAL;
  }
  if (mode_ == UI_MODE_SELECT_SCALE || mode_ == UI_MODE_DISPLAY_RESET_MODE) {
    if (queue_.idle_time() > 4000) {
      mode_ = UI_MODE_NORMAL;
      queue_.Touch();
    }
  } else if (queue_.idle_time() > 1000) {
    queue_.Touch();
  }
}

uint8_t Ui::HandleFactoryTestingRequest(uint8_t command) {
  uint8_t argument = command & 0x1f;
  command = command >> 5;
  uint8_t reply = 0;
  switch (command) {
    case FACTORY_TESTING_READ_POT:
    case FACTORY_TESTING_READ_CV:
      reply = cv_reader_->adc_value(argument);
      break;
    
    case FACTORY_TESTING_READ_NORMALIZATION:
      reply = clock_inputs_->is_normalized(ClockInput(argument)) ? 255 : 0;
      break;      
    
    case FACTORY_TESTING_READ_GATE:
      reply = argument >= SWITCH_LAST
          ? clock_inputs_->value(ClockInput(argument - SWITCH_LAST))
          : switches_.pressed(Switch(argument));
      break;
      
    case FACTORY_TESTING_GENERATE_TEST_SIGNALS:
      output_test_mode_ = static_cast<bool>(argument);
      fill(
          &output_test_forced_dac_code_[0],
          &output_test_forced_dac_code_[4],
          0);
      break;
      
    case FACTORY_TESTING_CALIBRATE:
      if (argument == 0) {
        // Revert all settings before getting into calibration mode.
        settings_->mutable_state()->t_deja_vu = 0;
        settings_->mutable_state()->x_deja_vu = 0;
        settings_->mutable_state()->t_model = 0;
        settings_->mutable_state()->t_range = 1;
        settings_->mutable_state()->x_control_mode = 0;
        settings_->mutable_state()->x_range = 2;
        settings_->mutable_state()->x_register_mode = 0;
        settings_->SavePersistentData();
        
        mode_ = UI_MODE_CALIBRATION_1;
      } else {
        NextCalibrationStep();
      }
      {
        const CalibrationData& cal = settings_->calibration_data();
        float voltage = (argument & 1) == 0 ? 1.0f : 3.0f;
        for (int i = 0; i < 4; ++i) {
          output_test_forced_dac_code_[i] =  static_cast<uint16_t>(
              voltage * cal.dac_scale[i] + cal.dac_offset[i]);
        }
      }
      queue_.Touch();
      break;
      
    case FACTORY_TESTING_FORCE_DAC_CODE:
      {
        int channel = argument >> 2;
        int step = argument & 0x3;
        if (step == 0) {
          output_test_forced_dac_code_[channel] = 0xaf35;
        } else if (step == 1) {
          output_test_forced_dac_code_[channel] = 0x1d98;
        } else {
          CalibrationData* cal = settings_->mutable_calibration_data();
          cal->dac_offset[channel] = static_cast<float>(
              calibration_data_ & 0xffff);
          cal->dac_scale[channel] = static_cast<float>(
              calibration_data_ >> 16) * -0.125f;
          output_test_forced_dac_code_[channel] = static_cast<uint16_t>(cal->dac_scale[channel] + cal->dac_offset[channel]);
          settings_->SavePersistentData();
        }
      }
      break;
      
    case FACTORY_TESTING_WRITE_CALIBRATION_DATA_NIBBLE:
      calibration_data_ <<= 4;
      calibration_data_ |= argument & 0xf;
      break;
  }
  return reply;
}

}  // namespace marbles
