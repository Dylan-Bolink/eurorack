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
const uint8_t* drum_map[5][5] = {
  { node_10, node_8, node_0, node_9, node_11 },
  { node_15, node_7, node_13, node_12, node_6 },
  { node_18, node_14, node_4, node_5, node_3 },
  { node_23, node_16, node_21, node_1, node_2 },
  { node_24, node_19, node_17, node_20, node_22 },
};

void PatternGenerator::Init() {
}

uint8_t PatternGenerator::ReadDrumMap(
    uint8_t step,
    uint8_t instrument,
    uint8_t x,
    uint8_t y) {
  uint8_t i = x >> 6;
  uint8_t j = y >> 6;

  const uint8_t* a_map = drum_map[i][j];
  const uint8_t* b_map = drum_map[i + 1][j];
  const uint8_t* c_map = drum_map[i][j + 1];
  const uint8_t* d_map = drum_map[i + 1][j + 1];
  
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