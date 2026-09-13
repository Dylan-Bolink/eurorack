// Copyright 2014 Emilie Gillet.
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
// System level initialization.

#include "warps/drivers/system.h"

namespace warps {

void System::Init(bool application) {
  if (application) {
    NVIC_SetVectorTable(NVIC_VectTab_FLASH, 0x8000);
  }

  // Flush-to-zero: denormals are inaudible here, and every feedback path in the
  // modulator (delay, smear, drift, comb, the one-poles) decays through them
  // whenever the input goes quiet.
  //
  // FPDSCR is the one that matters. With FPCCR.ASPEN set -- it is, by reset --
  // the hardware loads FPSCR from FPDSCR on exception entry, and all the audio
  // DSP runs inside the codec DMA interrupt. Setting only FPSCR here would
  // leave the audio path untouched. FPSCR is set as well, for the main loop.
  FPU->FPDSCR |= FPU_FPDSCR_FZ_Msk;
  __set_FPSCR(__get_FPSCR() | FPU_FPDSCR_FZ_Msk);
}

void System::StartTimers() {
  SysTick_Config(F_CPU / 1000);
}

}  // namespace warps
