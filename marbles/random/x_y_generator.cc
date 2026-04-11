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

namespace marbles {

using namespace std;
using namespace stmlib;

const int kChordNumChords = 12;
const int kChordNumNotes = 4;

// 12 chords
const float chords[kChordNumChords][kChordNumNotes] = {
  { 0.0f,  3.0f,  6.0f,  9.0f },   // dim7
  { 0.0f,  3.0f,  6.0f, 10.0f },   // half-dim
  { 0.0f,  3.0f,  7.0f, 10.0f },   // m7
  { 0.0f,  3.0f,  7.0f, 11.0f },   // mMaj7
  { 0.0f,  3.0f,  7.0f, 12.0f },   // min triad
  { 0.0f,  5.0f,  7.0f, 10.0f },   // sus4
  { 0.0f,  5.0f, 10.0f, 15.0f },   // quartal
  { 0.0f,  2.0f,  7.0f, 14.0f },   // sus2
  { 0.0f,  7.0f, 12.0f, 19.0f },   // power open fith
  { 0.0f,  4.0f,  7.0f, 12.0f },   // maj triad
  { 0.0f,  4.0f,  7.0f, 10.0f },   // Dom7
  { 0.0f,  4.0f,  7.0f, 11.0f },   // Maj7
};

// 8 pure 7th chords (Harmonaig style), ordered minor<->major
// const float chords[kChordNumChords][kChordNumNotes] = {
//   { 0.0f,  3.0f,  6.0f,  9.0f },   // dim7
//   { 0.0f,  3.0f,  6.0f, 10.0f },   // half-dim (m7b5)
//   { 0.0f,  3.0f,  7.0f, 10.0f },   // m7
//   { 0.0f,  3.0f,  7.0f, 11.0f },   // mMaj7
//   { 0.0f,  4.0f,  7.0f, 10.0f },   // Dom7
//   { 0.0f,  4.0f,  7.0f, 11.0f },   // Maj7
//   { 0.0f,  4.0f,  8.0f, 11.0f },   // augMaj7
//   { 0.0f,  4.0f,  8.0f, 10.0f },   // aug7
// };


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
  for (int i = 0; i < kNumStoredScales; ++i) {
    stored_scales_[i].Init();
  }
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

  if (*reset) {
    ramp_divider_.Reset();
    rr_counter_ = 0;
    rr_prev_phase_ = 0.0f;
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
    if (settings.control_mode == CONTROL_MODE_CHORD) {
      channel.set_steps(0.75f);  // amount=0.5 → all diatonic degrees
    } else {
      channel.set_steps(0.5f + (settings.steps - 0.5f) * \
          (settings.register_mode ? 1.0f : amount));
    }
    channel.set_scale_index(settings.scale_index > 5 ? 0 : settings.scale_index);
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
        && i > 0 && i < kNumXChannels) {
      sequence = &random_sequence_[0];
      if (settings.use_shift_register) {
        use_shifted_sequences = true;

        if (settings.control_mode == CONTROL_MODE_IDENTICAL
            || settings.control_mode == CONTROL_MODE_ROUND_ROBIN
            || settings.control_mode == CONTROL_MODE_CHORD) {
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
    
    if (x_settings.control_mode == CONTROL_MODE_CHORD && i < kNumXChannels) {
      // Chord mode: skip randomness
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

  if (x_settings.control_mode == CONTROL_MODE_CHORD) {
    bool chromatic = (x_settings.scale_index > 5);
    float spread_centered = x_settings.spread - 0.5f;
    if (spread_centered > -0.05f && spread_centered < 0.05f) spread_centered = 0.0f;
    float transpose = spread_centered * 2.0f;

    // Inversion 0-3 from X Range switch
    int inversion = static_cast<int>(x_settings.voltage_range);
    CONSTRAIN(inversion, 0, 3);

    // Steps chord quality index
    int chord_idx = static_cast<int>(x_settings.steps * (kChordNumChords - 1) + 0.5f);
    CONSTRAIN(chord_idx, 0, kChordNumChords - 1);

    for (size_t s = 0; s < size; s++) {
      size_t base = s * kNumChannels;

      float root = 10.0f * (x_settings.register_value - 0.5f);
      root += transpose;
      // root += 1.15f / 12.0f;  // compensate hardware ADC offset

      // Build voices
      float voices[3];
      float base_interval;
      if (chromatic) {
        root = roundf(root * 12.0f) / 12.0f;
        base_interval = 1.0f;
        for (int v = 0; v < 3; v++)
          voices[v] = root + chords[chord_idx][v + 1] / 12.0f;
      } else {
        base_interval = stored_scales_[x_settings.scale_index].base_interval;
        root = output_channel_[0].Quantize(root, 0.5f);
        for (int v = 0; v < 3; v++)
          voices[v] = output_channel_[0].Quantize(root + chords[chord_idx][v + 1] / 12.0f, 0.5f);
      }

      // Spread: CCW=closed, noon=closed, past noon=drop2, CW=open
      int voicing = 0;
      if (x_settings.bias > 0.75f) voicing = 3;      // open
      else if (x_settings.bias > 0.5f) voicing = 1;  // drop2

      switch (voicing) {
        case 1: // Drop 2
          voices[1] -= base_interval;
          if (voices[1] < voices[0]) { float t = voices[0]; voices[0] = voices[1]; voices[1] = t; }
          break;
        case 2: // Drop 3
          voices[0] -= base_interval;
          break;
        case 3: // Open: drop 2 + raise highest
          voices[1] -= base_interval;
          if (voices[1] < voices[0]) { float t = voices[0]; voices[0] = voices[1]; voices[1] = t; }
          voices[2] += base_interval;
          break;
        default:
          break;
      }

      // Apply inversion
      for (int inv = 0; inv < inversion; inv++) {
        voices[0] += base_interval;
        if (voices[0] > voices[1]) { float t = voices[0]; voices[0] = voices[1]; voices[1] = t; }
        if (voices[1] > voices[2]) { float t = voices[1]; voices[1] = voices[2]; voices[2] = t; }
        if (voices[0] > voices[1]) { float t = voices[0]; voices[0] = voices[1]; voices[1] = t; }
      }

      output[base + 3] = root;
      output[base + 0] = voices[0];
      output[base + 1] = voices[1];
      output[base + 2] = voices[2];
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