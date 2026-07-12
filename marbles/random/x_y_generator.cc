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

  acid_align_step_ = 0;
  prev_clock_source_ = -1;
  prev_control_mode_ = CONTROL_MODE_IDENTICAL;
  acid_pending_loop_align_ = false;
  acid_prev_tick_step_ = 0;
  acid_pitch_ = 2.0f;
  acid_slide_target_ = 2.0f;
  acid_prev_target_ = 2.0f;
  ResetAcidPhase();
}

void XYGenerator::ResetAcidPhase() {
  acid_prev_ramp_ = 0.0f;
  // Phase-lock the half-time divider so acid steps land on even Grids steps.
  // (Outside Grids mode half_time is off and acid_div_ is unused.)
  acid_div_ = (acid_align_step_ & 1) ? 0 : 1;
  acid_gate_ = false;
  acid_accent_ = false;
  acid_slide_ = false;
  acid_prev_accent_ = false;
}

namespace {

// Deterministic sub-draws from a single loop value: multiplying by a prime
// and keeping the fraction yields decorrelated decisions per step while
// remaining stable under deja vu looping.
inline float Fraction(float x) {
  return x - static_cast<float>(static_cast<int>(x));
}

}  // namespace

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

  if (x_settings.control_mode == CONTROL_MODE_ACID) {
    float* acid_clock;
    if (clock_source == CLOCK_SOURCE_EXTERNAL) {
      acid_clock = ramps.slave[0];
    } else if (clock_source == CLOCK_SOURCE_INTERNAL_T1_T2_T3) {
      acid_clock = ramps.master;
    } else {
      acid_clock = channel_ramp[0];
    }
    channel_ramp[0] = channel_ramp[1] = channel_ramp[2] = acid_clock;
  }

  // A clock-source change (patching/unpatching the X clock, the self-
  // patching detector, or the boot-time normalization flap) points the
  // channel ramps at different buffers. Comparing the previous-phase
  // trackers across buffers reads a phantom wrap and fires a spurious
  // step, envelope or rotation — re-sync them to the new ramps instead;
  // the next event is then a genuine wrap.
  if (static_cast<int>(clock_source) != prev_clock_source_) {
    prev_clock_source_ = static_cast<int>(clock_source);
    acid_prev_ramp_ = channel_ramp[0][0];
    rr_prev_phase_ = channel_ramp[0][0];
    for (size_t ch = 0; ch < kNumXChannels; ++ch) {
      env_prev_ramp_[ch] = channel_ramp[ch][0];
      output_channel_[ch].set_previous_phase(channel_ramp[ch][0]);
    }
  }

  // Re-seed state on explicit reset, and re-sync the trackers when the user
  // switches X mode mid-run: stale phases from an earlier stint (or Init)
  // would read a phantom wrap and fire a spurious step or envelope on the
  // first block in the new mode — no explicit reset needed to fix it.
  const bool x_mode_changed = x_settings.control_mode != prev_control_mode_;
  const bool entering_acid =
      x_mode_changed && x_settings.control_mode == CONTROL_MODE_ACID;
  prev_control_mode_ = x_settings.control_mode;

  if (*reset) {
    ramp_divider_.Reset();
    rr_counter_ = 0;
    rr_prev_phase_ = 0.0f;
    fill(&env_phase_[0], &env_phase_[kNumXChannels], 1.0f);
    fill(&env_prev_ramp_[0], &env_prev_ramp_[kNumXChannels], 0.0f);
    ResetAcidPhase();
  } else if (x_mode_changed) {
    rr_prev_phase_ = channel_ramp[0][0];
    fill(&env_phase_[0], &env_phase_[kNumXChannels], 1.0f);
    for (size_t ch = 0; ch < kNumXChannels; ++ch) {
      env_prev_ramp_[ch] = channel_ramp[ch][0];
      // Envelope/acid stints skip channel.Process for the X trio, freezing
      // the OutputChannel phases; re-sync them so returning to a standard
      // mode can't emit an unclocked random step.
      output_channel_[ch].set_previous_phase(channel_ramp[ch][0]);
    }
    if (entering_acid) {
      ResetAcidPhase();
      // In Grids, snap the riff loop to its top at the next drum bar-start
      // so the acid loop drops in on the downbeat; then let it free-run.
      if (x_settings.acid_half_time) {
        acid_pending_loop_align_ = true;
        acid_prev_tick_step_ = acid_align_step_;
      }
    }
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
        && x_settings.control_mode != CONTROL_MODE_ENVELOPE
        && x_settings.control_mode != CONTROL_MODE_ACID) {
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
    
    if ((x_settings.control_mode == CONTROL_MODE_ENVELOPE
         || x_settings.control_mode == CONTROL_MODE_ACID)
        && i < kNumXChannels) {
      // Envelope/acid modes render the X trio themselves below.
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

          // 0 = hard reset, 1 = only after attack (serge), 2 = legato
          if (x_settings.envelope_retrigger == 1) {
            if (phase < af) triggered = false;
          } else if (x_settings.envelope_retrigger == 2) {
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
          time_value = 0.3f + time_value * 0.7f;

          env_rate_[ch] = (1.0f / 32.0f)
              * SemitonesToRatioSafe(-180.0f * time_value);
          env_phase_[ch] = 0.0f;
        }

        // Compute envelope shape using phase-warped raised cosine
        float value = 0.0f;
        float phase = env_phase_[ch];

        if (phase < 1.0f) {
          float attack_fraction = steps;
          CONSTRAIN(attack_fraction, 0.001f, 0.9f);

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

  if (x_settings.control_mode == CONTROL_MODE_ACID) {
    const float* clock_ramp = channel_ramp[0];
    // Half-time only compensates the 2x master ramp used in Grids mode. A
    // self-patched T1/T3 clock resolves to a slave ramp that is not 2x, so
    // gate half-time on the clock actually being the master ramp.
    const bool half_time = x_settings.acid_half_time
        && clock_ramp == ramps.master;

    // Steps: gate density. A small dead zone at the very bottom silences the
    // line completely; above it, the rest of the knob spans the original
    // TB-3PO curve (10%..100%).
    const float kDensityDeadZone = 0.02f;
    float gate_prob;
    if (x_settings.steps < kDensityDeadZone) {
      gate_prob = 0.0f;
    } else {
      const float d = (x_settings.steps - kDensityDeadZone)
          / (1.0f - kDensityDeadZone);
      gate_prob = 0.1f + 0.9f * d;
    }

    // Bias: slide/accent balance. Noon = authentic TB-303 probabilities;
    // CCW pushes slides towards "always", CW does the same for accents.
    float slide_prob = 0.18f;
    float accent_prob = 0.16f;
    if (x_settings.bias < 0.5f) {
      slide_prob += (0.5f - x_settings.bias) * 2.0f * (1.0f - 0.18f);
    } else {
      accent_prob += (x_settings.bias - 0.5f) * 2.0f * (1.0f - 0.16f);
    }

    // Spread: pitch pool via the weight-aware quantizer (root only ->
    // root+fifth -> triad -> diatonic -> chromatic), note repeats fading
    // out, and octave jumps appearing at the top of the range.
    const float spread = x_settings.spread;
    const float quantize_amount = 1.0f - spread * 0.8f;
    float repeat_prob = 0.5f - spread * 0.9f;
    if (repeat_prob < 0.0f) repeat_prob = 0.0f;
    float octave_prob = (spread - 0.75f) * 1.6f;
    CONSTRAIN(octave_prob, 0.0f, 0.4f);

    // X Range hold: momentary acid fill, a randomly-chosen flavor that
    // overrides the relevant parameters while active, then snaps back.
    bool octave_run = false;
    bool roll_fill = false;
    if (x_settings.acid_fill_active) {
      switch (x_settings.acid_fill_flavor) {
        case 0:  // Roll: every step gated, accents alternating.
          gate_prob = 1.0f;
          roll_fill = true;
          break;
        case 1:  // Octave run: frequent upward leaps, always new notes.
          octave_prob = 0.6f;
          repeat_prob = 0.0f;
          octave_run = true;
          // Floor the density: runs only shape notes that play, so a fill
          // must guarantee enough of them to be heard at sparse Steps.
          if (gate_prob < 0.75f) gate_prob = 0.75f;
          break;
        default:  // Slide run: full legato.
          slide_prob = 1.0f;
          if (gate_prob < 0.75f) gate_prob = 0.75f;
          break;
      }
    }

    for (size_t s = 0; s < size; ++s) {
      const float ph = clock_ramp[s];
      const bool wrap = ph < acid_prev_ramp_;
      acid_prev_ramp_ = ph;

      if (wrap) {
        // Half-time parity comes from the actual drum step (one step per
        // master wrap, already advanced for this block), not a free-running
        // toggle: deja vu loops with odd lengths jump the step counter
        // backwards, and a toggle would drift the line a 32nd off the drum
        // grid on every loop pass.
        acid_div_ = half_time
            ? ((acid_align_step_ & 1) ? 0 : 1)
            : acid_div_ ^ 1;
        if (!half_time || acid_div_ == 1) {
          // --- Step tick ---
          // One-shot loop-start align: on the first acid step after the Grids
          // bar wraps (drum step goes backwards), reset the sequences so this
          // step plays the riff's step 0, landing the loop on the downbeat.
          if (acid_pending_loop_align_
              && acid_align_step_ < acid_prev_tick_step_) {
            for (size_t ch = 0; ch < kNumXChannels; ++ch) {
              random_sequence_[ch].Reset();
            }
            acid_pending_loop_align_ = false;
          }
          acid_prev_tick_step_ = acid_align_step_;

          // One value per stream per step keeps the deja vu loop length
          // exact: stream 0 -> pitch, 1 -> gate/accent, 2 -> slide/octave.
          const float u_pitch = random_sequence_[0].NextValue(false, 0.0f);
          const float u_gate = random_sequence_[1].NextValue(false, 0.0f);
          const float u_mod = random_sequence_[2].NextValue(false, 0.0f);

          const bool gated = u_gate < gate_prob;
          const bool tie = acid_slide_;  // previous step slides into this one

          // 303 rule: slides and accents are rarer twice in a row. The
          // suppression fades out as the knob approaches "always".
          float accent_suppression = 1.0f;
          if (acid_prev_accent_) {
            accent_suppression = 0.45f + 0.55f * (accent_prob - 0.16f) / 0.84f;
          }
          // Roll fill: alternate accents so the stutter pumps instead of
          // sitting at a constant 5V (all-accented = no dynamics at all).
          const bool accent = roll_fill
              ? (gated && !acid_prev_accent_)
              : gated
                  && Fraction(u_gate * 61.0f)
                      < accent_prob * accent_suppression;
          float slide_suppression = 1.0f;
          if (tie) {
            slide_suppression = 0.55f + 0.45f * (slide_prob - 0.18f) / 0.82f;
          }
          // Only gated steps can slide: a slide ties a note into the next
          // step, so rolling it on a rest would fill rests and defeat the
          // density control (and never go silent in the dead zone).
          acid_slide_ = gated
              && Fraction(u_mod * 61.0f) < slide_prob * slide_suppression;
          acid_prev_accent_ = accent;

          // New note, or repeat of the previous one. Notes live in a
          // root-centered octave; the quantizer's weight thresholds turn
          // spread into the size of the available pitch pool.
          float target = acid_prev_target_;
          if (Fraction(u_pitch * 61.0f) >= repeat_prob) {
            if (x_settings.chromatic) {
              // Chromatic: round to the nearest semitone within an octave.
              const float semis = (u_pitch - 0.5f) * 12.0f;
              const int st = static_cast<int>(
                  semis + (semis >= 0.0f ? 0.5f : -0.5f));
              target = 2.0f + static_cast<float>(st) / 12.0f;
            } else {
              target = 2.0f + output_channel_[0].Quantize(
                  u_pitch - 0.5f, quantize_amount);
            }
            // Fixed +/-1 octave jumps, driven by Spread (octave_prob). The
            // Octave-run fill forces every jump upward.
            const float d_oct = Fraction(u_mod * 3907.0f);
            if (d_oct < octave_prob) {
              target += octave_run
                  ? 1.0f
                  : (d_oct * 2.0f < octave_prob ? 1.0f : -1.0f);
            }
          }
          acid_prev_target_ = target;

          if (tie) {
            acid_slide_target_ = target;
          } else if (gated) {
            acid_pitch_ = target;
            acid_slide_target_ = target;
          }

          if (gated || tie) {
            acid_gate_ = true;
            acid_accent_ = accent;
          } else {
            acid_gate_ = false;
            acid_accent_ = false;
          }
        }
      }

      // Phase within the current step (half-time in Grids mode, whose
      // master clock runs at 2x).
      float step_phase = ph;
      if (half_time) {
        step_phase = acid_div_ == 1 ? 0.5f * ph : 0.5f + 0.5f * ph;
      }

      // 303 gate timing: half the step, but a slid step holds its gate
      // through the transition into the next one.
      if (acid_gate_ && step_phase >= 0.5f && !acid_slide_) {
        acid_gate_ = false;
        acid_accent_ = false;
      }

      // Fixed-time exponential pitch slide.
      if (acid_pitch_ != acid_slide_target_) {
        acid_pitch_ += 0.003f * (acid_slide_target_ - acid_pitch_);
        float remaining = acid_slide_target_ - acid_pitch_;
        if (remaining < 0.0001f && remaining > -0.0001f) {
          acid_pitch_ = acid_slide_target_;
        }
      }

      output[0 + s * kNumChannels] = acid_pitch_;

      // X2: gates, 3V normal / 5V accent.
      const float gate_level = acid_accent_ ? 5.0f : 3.0f;
      output[1 + s * kNumChannels] = acid_gate_ ? gate_level : 0.0f;

      // X3: accent-only gate.
      output[2 + s * kNumChannels] =
          (acid_gate_ && acid_accent_) ? 5.0f : 0.0f;
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
  } else {
    // Keep the hold registers tracking whatever each X output last emitted,
    // so entering Round Robin mid-run holds the current voltages instead of
    // stale ones from a previous stint (or 0V on the very first entry).
    for (size_t ch = 0; ch < kNumXChannels; ++ch) {
      rr_held_[ch] = output[ch + (size - 1) * kNumChannels];
    }
  }
}

}  // namespace marbles