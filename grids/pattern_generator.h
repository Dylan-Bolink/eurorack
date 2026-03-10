// Copyright 2011 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// -----------------------------------------------------------------------------
//
// Pattern generator.
//
// OUTPUT MODE  OUTPUT CLOCK  BIT7  BIT6  BIT5  BIT4  BIT3  BIT2  BIT1  BIT0
// DRUMS        FALSE          RND   CLK  HHAC  SDAC  BDAC    HH    SD    BD
// DRUMS        TRUE           RND   CLK   CLK   BAR   ACC    HH    SD    BD
// EUCLIDEAN    FALSE          RND   CLK  RST3  RST2  RST1  EUC3  EUC2  EUC1
// EUCLIDEAN    TRUE           RND   CLK   CLK  STEP   RST  EUC3  EUC2  EUC1
#ifndef GRIDS_PATTERN_GENERATOR_H_
#define GRIDS_PATTERN_GENERATOR_H_

#include <cstring>
#include <stdint.h>
#include <stdlib.h>

#include "stmlib/stmlib.h"

// --- STM32 COMPATIBILITY MACROS ---
#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif

namespace avrlib {
  class Random {
   public:
    static uint8_t GetByte() { return rand() & 0xFF; }
  };
}

namespace grids {

const uint8_t kStepsPerPattern = 32;

class PatternGenerator {
 public:
  PatternGenerator() { }
  ~PatternGenerator() { }

  void Init();
  
  struct OutputStep {
    uint8_t bd;
    uint8_t sd;
    uint8_t hh;
    uint8_t accent;
  };

  OutputStep GetStep(uint8_t step, uint8_t x, uint8_t y);
  uint8_t ReadDrumMap(uint8_t step, uint8_t instrument, uint8_t x, uint8_t y);

  void SetBank(uint8_t bank) { bank_ = bank < 3 ? bank : 2; }
  void SetHenri(bool henri) { henri_ = henri; }

 private:

  uint8_t bank_;
  bool henri_;

  DISALLOW_COPY_AND_ASSIGN(PatternGenerator);
};

}  // namespace grids

#endif // GRIDS_PATTERN_GENERATOR_H_
