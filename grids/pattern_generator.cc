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
// Global clock.

#include "grids/pattern_generator.h"
#include "grids/resources.h"

#include <algorithm>

namespace grids {

static inline uint8_t U8Mix(uint8_t a, uint8_t b, uint8_t balance) {
  uint16_t t = a * (255 - balance) + b * balance;
  return t >> 8;
}



/* static */
const uint8_t* drum_map[3][5][5] = {
  // Bank 0: OG Grids (curated layout)
  {
    { node_10, node_8, node_0, node_9, node_11 },
    { node_15, node_7, node_13, node_12, node_6 },
    { node_18, node_14, node_4, node_5, node_3 },
    { node_23, node_16, node_21, node_1, node_2 },
    { node_24, node_19, node_17, node_20, node_22 },
  },
  // Bank 1: Electronic
  {
    { node_25, node_26, node_27, node_28, node_29 },
    { node_30, node_31, node_32, node_33, node_34 },
    { node_35, node_36, node_37, node_38, node_39 },
    { node_40, node_41, node_42, node_43, node_44 },
    { node_45, node_46, node_47, node_48, node_49 },
  },
  // Bank 2: Breakbeat
  {
    { node_50, node_51, node_52, node_53, node_54 },
    { node_55, node_56, node_57, node_58, node_59 },
    { node_60, node_61, node_62, node_63, node_64 },
    { node_65, node_66, node_67, node_68, node_69 },
    { node_70, node_71, node_72, node_73, node_74 },
  },
};

void PatternGenerator::Init() {
  bank_ = 0;
}

uint8_t PatternGenerator::ReadDrumMap(
    uint8_t step,
    uint8_t instrument,
    uint8_t x,
    uint8_t y) {
  uint8_t i = x >> 6;
  uint8_t j = y >> 6;

  const uint8_t* a_map = drum_map[bank_][i][j];
  const uint8_t* b_map = drum_map[bank_][i + 1][j];
  const uint8_t* c_map = drum_map[bank_][i][j + 1];
  const uint8_t* d_map = drum_map[bank_][i + 1][j + 1];
  
  uint8_t offset = (instrument * kStepsPerPattern) + step;
  
  uint8_t a = pgm_read_byte(a_map + offset);
  uint8_t b = pgm_read_byte(b_map + offset);
  uint8_t c = pgm_read_byte(c_map + offset);
  uint8_t d = pgm_read_byte(d_map + offset);
  
  return U8Mix(U8Mix(a, b, x << 2), U8Mix(c, d, x << 2), y << 2);
}

// New function for Marbles
PatternGenerator::OutputStep PatternGenerator::GetStep(uint8_t step, uint8_t x, uint8_t y) {
    PatternGenerator::OutputStep result;

    result.bd = ReadDrumMap(step, 0, x, y);
    result.sd = ReadDrumMap(step, 1, x, y);
    result.hh = ReadDrumMap(step, 2, x, y);
    
    uint8_t max_val = 0;
    if (result.bd > max_val) max_val = result.bd;
    if (result.sd > max_val) max_val = result.sd;
    if (result.hh > max_val) max_val = result.hh;
    
    result.accent = (max_val > 192) ? 1 : 0;
    
    return result;
}

}  // namespace grids