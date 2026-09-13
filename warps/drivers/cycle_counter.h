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
// Free-running core cycle counter (Cortex-M4 DWT unit).
//
// Reading it costs a single load, so it is cheap enough to bracket the audio
// callback with. The counter is 32 bits and wraps every 2^32 / 168e6 = 25.6 s,
// but differences taken with unsigned arithmetic stay correct across a wrap as
// long as the measured interval is shorter than that — which any audio block
// comfortably is.

#ifndef WARPS_DRIVERS_CYCLE_COUNTER_H_
#define WARPS_DRIVERS_CYCLE_COUNTER_H_

#include "stmlib/stmlib.h"

#ifndef TEST
#include <stm32f4xx_conf.h>
#endif

namespace warps {

class CycleCounter {
 public:
#ifdef TEST
  static void Init() { }
  static inline uint32_t Read() { return 0; }
#else
  static void Init() {
    // TRCENA gates the whole trace/debug block, DWT included.
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  }
  static inline uint32_t Read() {
    return DWT->CYCCNT;
  }
#endif

 private:
  DISALLOW_COPY_AND_ASSIGN(CycleCounter);
};

}  // namespace warps

#endif  // WARPS_DRIVERS_CYCLE_COUNTER_H_
