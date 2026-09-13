// Copyright 2024 Dylan Bolink.
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
// Per-feature-mode CPU profiler.
//
// Accumulates cycle counts for the audio callback, bucketed by the feature mode
// that was active, so sweeping through the modes fills in a table of what each
// one actually costs. Recording happens inside the audio interrupt and is a
// handful of instructions per block; the (slow) UART reporting happens from the
// main loop, where blocking is harmless because audio runs off the codec DMA
// interrupt.
//
// Everything is deliberately 32-bit. A 64-bit accumulator would need
// __aeabi_uldivmod to average, and that libgcc object carries an .ARM.exidx
// section which stmlib's linker script has nowhere to put. Averages are
// therefore taken over a reporting window rather than all time; peaks and
// overrun counts stay cumulative, which is what you actually want to watch.
//
// The stats live in a fixed global in CCM RAM, so they can equally be read
// straight out of memory with a debug probe instead of over the serial port.

#ifndef WARPS_PROFILER_H_
#define WARPS_PROFILER_H_

// ---------------------------------------------------------------------------
// Set to 1 to build the CPU profiler, 0 for a production build.
// This is the only switch; warps.cc and ui.cc both read it.
// ---------------------------------------------------------------------------
#define PROFILE_CPU 1

#include "stmlib/stmlib.h"

#include "warps/drivers/cycle_counter.h"
#include "warps/drivers/debug_port.h"
#include "warps/dsp/parameters.h"

namespace warps {

const size_t kNumFeatureModes = FEATURE_MODE_META + 1;

// One bucket per feature mode. POD, at a stable address, so it can be dumped
// with a debug probe.
struct ModeProfile {
  // Reset after every report; the mean is taken over this window.
  uint32_t window_modulator_cycles;
  uint32_t window_callback_cycles;
  uint32_t window_blocks;
  uint32_t window_callback_peak;  // drives the LED, so it tracks live
  uint32_t window_overruns;
  // Cumulative since the last Reset().
  uint32_t modulator_peak;
  uint32_t callback_peak;
  uint32_t overruns;      // blocks whose callback exceeded the budget
  uint32_t total_blocks;
};

struct ProfileData {
  ModeProfile mode[kNumFeatureModes];
  uint32_t cycle_budget;  // cycles available per block
  uint32_t block_size;
  uint32_t sample_rate;
};

// Defined in profiler.cc, in CCM RAM. Dump this symbol with a debug probe to
// read the numbers without a serial adapter.
extern ProfileData g_profile;

class Profiler {
 public:
  Profiler() { }
  ~Profiler() { }

  void Init(DebugPort* debug_port, uint32_t sample_rate, size_t block_size);

  // --- called from the audio interrupt ---

  inline void BeginBlock() {
    callback_start_ = CycleCounter::Read();
  }

  inline void BeginModulator() {
    modulator_start_ = CycleCounter::Read();
  }

  inline void EndModulator() {
    modulator_elapsed_ = CycleCounter::Read() - modulator_start_;
  }

  // Buckets this block against the mode that was actually running.
  inline void EndBlock(FeatureMode feature_mode) {
    uint32_t callback_elapsed = CycleCounter::Read() - callback_start_;
    if (static_cast<size_t>(feature_mode) >= kNumFeatureModes) {
      return;
    }
    ModeProfile* p = &g_profile.mode[feature_mode];
    p->window_modulator_cycles += modulator_elapsed_;
    p->window_callback_cycles += callback_elapsed;
    ++p->window_blocks;
    ++p->total_blocks;
    if (modulator_elapsed_ > p->modulator_peak) {
      p->modulator_peak = modulator_elapsed_;
    }
    if (callback_elapsed > p->callback_peak) {
      p->callback_peak = callback_elapsed;
    }
    if (callback_elapsed > p->window_callback_peak) {
      p->window_callback_peak = callback_elapsed;
    }
    if (callback_elapsed > g_profile.cycle_budget) {
      ++p->overruns;
      ++p->window_overruns;
    }
    ++blocks_since_report_;
  }

  // --- called from the main loop ---

  // Emits a table over the debug UART once per reporting interval. Blocking,
  // but only on the main loop.
  void DoEvents();

  // Clears every counter, peaks included.
  void Reset();

  // Renders the current mode's load onto the main RGB LED, so the numbers can
  // be read off the module itself with no serial adapter or debug probe.
  // See profiler.cc for the blink encoding.
  void RenderLed(FeatureMode feature_mode, uint8_t* rgb);

 private:
  void Print(const char* s);
  void PrintPadded(const char* s, int width);
  void PrintUnsigned(uint32_t value, int width);
  void PrintReport();

  DebugPort* debug_port_;
  uint32_t callback_start_;
  uint32_t modulator_start_;
  uint32_t modulator_elapsed_;
  uint32_t blocks_since_report_;
  uint32_t blocks_per_report_;

  DISALLOW_COPY_AND_ASSIGN(Profiler);
};

}  // namespace warps

#if PROFILE_CPU
// The single instance, defined at global scope in warps.cc alongside the other
// module singletons (codec, ui, modulator...).
extern warps::Profiler profiler;
#endif  // PROFILE_CPU

#endif  // WARPS_PROFILER_H_
