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

#include "warps/profiler.h"

#include "stmlib/system/system_clock.h"

namespace warps {

// Main RAM is essentially full (the delay buffers own it), so this lives in the
// otherwise unused 64K of core-coupled RAM at 0x10000000. CCM is core-only --
// no DMA reaches it -- which suits profiler counters exactly. The section is
// NOLOAD, so Init() zeroes it explicitly rather than relying on startup code.
//
// A debug probe can read it by name:
//   (gdb) p g_profile
//   (gdb) p g_profile.mode[1]        // FEATURE_MODE_CASSETTE_MIXER
ProfileData g_profile __attribute__((section(".ccmdata")));

namespace {

const char* const mode_names[kNumFeatureModes] = {
  "Crush",
  "Cassette",
  "Lossy",
  "Radio",
  "Dreamy",
  "FreqShift",
  "BitMangle",
  "Delay",
  "Meta",
};

}  // namespace

void Profiler::Init(
    DebugPort* debug_port,
    uint32_t sample_rate,
    size_t block_size) {
  debug_port_ = debug_port;
  callback_start_ = 0;
  modulator_start_ = 0;
  modulator_elapsed_ = 0;
  blocks_since_report_ = 0;

  g_profile.sample_rate = sample_rate;
  g_profile.block_size = block_size;
  // Cycles the CPU gets per block before the next one is due. Grouped to stay
  // in 32-bit arithmetic: 168000000 / 96000 * 60 = 105000.
  g_profile.cycle_budget = (F_CPU / sample_rate) * block_size;

  // Report roughly every 2 seconds.
  blocks_per_report_ = sample_rate * 2 / block_size;

  Reset();
  CycleCounter::Init();
}

void Profiler::Reset() {
  __disable_irq();
  for (size_t i = 0; i < kNumFeatureModes; ++i) {
    ModeProfile* m = &g_profile.mode[i];
    m->window_modulator_cycles = 0;
    m->window_callback_cycles = 0;
    m->window_blocks = 0;
    m->window_callback_peak = 0;
    m->window_overruns = 0;
    m->modulator_peak = 0;
    m->callback_peak = 0;
    m->overruns = 0;
    m->total_blocks = 0;
  }
  blocks_since_report_ = 0;
  __enable_irq();
}

void Profiler::Print(const char* s) {
  while (*s) {
    while (!debug_port_->writable());
    debug_port_->Overwrite(static_cast<uint8_t>(*s++));
  }
}

// Left-aligned, space-padded to width. Counts as it goes, so no strlen() call
// is needed -- any real libc reference would drag .ARM.exidx into the link.
void Profiler::PrintPadded(const char* s, int width) {
  int n = 0;
  while (*s) {
    while (!debug_port_->writable());
    debug_port_->Overwrite(static_cast<uint8_t>(*s++));
    ++n;
  }
  while (n++ < width) {
    while (!debug_port_->writable());
    debug_port_->Overwrite(' ');
  }
}

// Right-aligned unsigned decimal. No printf: it would pull in a large chunk of
// newlib and a lot of stack for no benefit here.
void Profiler::PrintUnsigned(uint32_t value, int width) {
  char buffer[12];
  int n = 0;
  do {
    buffer[n++] = '0' + (value % 10);
    value /= 10;
  } while (value && n < 11);

  for (int i = n; i < width; ++i) {
    while (!debug_port_->writable());
    debug_port_->Overwrite(' ');
  }
  while (n--) {
    while (!debug_port_->writable());
    debug_port_->Overwrite(static_cast<uint8_t>(buffer[n]));
  }
}

void Profiler::PrintReport() {
  const uint32_t budget = g_profile.cycle_budget;
  const uint32_t percent_divisor = budget / 100;

  Print("\r\n=== Warps Artifacts CPU profile ===\r\n");
  PrintUnsigned(g_profile.sample_rate, 0);
  Print(" Hz, ");
  PrintUnsigned(g_profile.block_size, 0);
  Print(" spl/block, budget ");
  PrintUnsigned(budget, 0);
  Print(" cyc/block\r\n");
  Print("mode       blocks  modAvg   modPk   cbAvg    cbPk  pk%  over\r\n");

  for (size_t i = 0; i < kNumFeatureModes; ++i) {
    // Snapshot one bucket with the audio interrupt masked, then clear the
    // window in the same critical section so no block is counted twice or lost.
    __disable_irq();
    const ModeProfile p = g_profile.mode[i];
    g_profile.mode[i].window_modulator_cycles = 0;
    g_profile.mode[i].window_callback_cycles = 0;
    g_profile.mode[i].window_blocks = 0;
    g_profile.mode[i].window_callback_peak = 0;
    g_profile.mode[i].window_overruns = 0;
    __enable_irq();

    if (!p.total_blocks) {
      continue;  // never selected since the last reset
    }

    PrintPadded(mode_names[i], 10);
    PrintUnsigned(p.total_blocks, 8);
    if (p.window_blocks) {
      PrintUnsigned(p.window_modulator_cycles / p.window_blocks, 8);
    } else {
      PrintPadded("       -", 8);
    }
    PrintUnsigned(p.modulator_peak, 8);
    if (p.window_blocks) {
      PrintUnsigned(p.window_callback_cycles / p.window_blocks, 8);
    } else {
      PrintPadded("       -", 8);
    }
    PrintUnsigned(p.callback_peak, 8);
    PrintUnsigned(percent_divisor ? p.callback_peak / percent_divisor : 0, 5);
    PrintUnsigned(p.overruns, 6);
    Print("\r\n");
  }
}

// --- LED readout ---------------------------------------------------------
//
// Blinks the current mode's peak load, as a percentage of the per-block budget,
// out of the main RGB LED:
//
//   green blinks  = tens digit      e.g. 73% -> 7 green, then 3 red
//   red blinks    = units digit
//   one blue blink = that digit is zero
//   fast red strobe = at or over 100%, or blocks were actually dropped
//
// The value tracks the last couple of seconds rather than all time, so it
// follows the knobs while you sweep a mode looking for its worst case.
//
// system_clock ticks at 1.6 kHz here (see the note in Ui::Poll), so one "ms"
// is 0.625 real ms; the constants below are in those ticks.
namespace {

const uint32_t kBlinkOn = 160;      // ~100 ms lit
const uint32_t kBlinkPeriod = 400;  // ~250 ms per blink
const uint32_t kDigitGap = 1200;    // ~750 ms between the two digits
const uint32_t kCycleGap = 3200;    // ~2 s before the reading repeats

}  // namespace

void Profiler::RenderLed(FeatureMode feature_mode, uint8_t* rgb) {
  rgb[0] = rgb[1] = rgb[2] = 0;
  if (static_cast<size_t>(feature_mode) >= kNumFeatureModes) {
    return;
  }
  const ModeProfile& p = g_profile.mode[feature_mode];
  if (!p.total_blocks) {
    return;  // nothing measured yet
  }

  const uint32_t percent_divisor = g_profile.cycle_budget / 100;
  const uint32_t load = percent_divisor
      ? p.window_callback_peak / percent_divisor
      : 0;
  const uint32_t t = stmlib::system_clock.milliseconds();

  if (p.window_overruns || load >= 100) {
    rgb[0] = (t & 127) < 64 ? 255 : 0;  // fast red strobe: over budget
    return;
  }

  const uint32_t tens = load / 10;
  const uint32_t units = load % 10;
  // A zero digit still gets one blink, shown in blue so it cannot be confused
  // with the gap either side of it.
  const uint32_t tens_len = (tens ? tens : 1) * kBlinkPeriod;
  const uint32_t units_len = (units ? units : 1) * kBlinkPeriod;
  const uint32_t phase = t % (tens_len + kDigitGap + units_len + kCycleGap);

  if (phase < tens_len) {
    if ((phase % kBlinkPeriod) < kBlinkOn) {
      if (tens) {
        rgb[1] = 255;  // green
      } else {
        rgb[2] = 255;  // blue
      }
    }
  } else if (phase >= tens_len + kDigitGap &&
             phase < tens_len + kDigitGap + units_len) {
    const uint32_t q = phase - tens_len - kDigitGap;
    if ((q % kBlinkPeriod) < kBlinkOn) {
      if (units) {
        rgb[0] = 255;  // red
      } else {
        rgb[2] = 255;  // blue
      }
    }
  }
}

void Profiler::DoEvents() {
  if (blocks_since_report_ < blocks_per_report_) {
    return;
  }
  blocks_since_report_ = 0;
  PrintReport();
}

}  // namespace warps
