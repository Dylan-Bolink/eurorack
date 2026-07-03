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
static const uint8_t* const drum_map[3][5][5] = {
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
    { node_39, node_26, node_25, node_29, node_33 },  // X=0: Synthwave→Deep House→4/4 House→Disco→Hard Techno (all 4otf)
    { node_30, node_31, node_27, node_28, node_34 },  // X=1: Min Techno→Detroit→Tech House→Acid House→Gabber
    { node_38, node_40, node_35, node_46, node_32 },  // X=2: Electro 808→Old Breaks→UK Garage→Grime→Industrial
    { node_45, node_36, node_43, node_42, node_48 },  // X=3: Dubstep→2-Step→Halftime DnB→DnB→Footwork
    { node_44, node_37, node_47, node_41, node_49 },  // X=4: IDM→Broken Beat→UK Bass→Jungle→Juke
  },
  // Bank 2: Breakbeat
  {
    { node_73, node_52, node_72, node_70, node_71 },  // X=0: Neo Soul→Jazz Rap→Soul/R&B→Old Funk→Breakbeat
    { node_53, node_50, node_51, node_54, node_74 },  // X=1: Lo-Fi→Classic BB→NY BB→Abstract HH→Jersey Club
    { node_55, node_59, node_56, node_57, node_58 },  // X=2: G-Funk→Snap→Hyphy→Bounce→Crunk
    { node_64, node_60, node_61, node_62, node_63 },  // X=3: Plugg→Trap→Hard Trap→Chi Drill→UK Drill
    { node_68, node_69, node_67, node_65, node_66 },  // X=4: Amapiano→Dancehall→Afrobeats→Reggaeton→Dembow
  },
};

void PatternGenerator::Init() {
  bank_ = 0;
  henri_ = false;
}

// Bank 3 step weight. Density (applied downstream as a threshold) reveals steps
// in weight order, so this defines the rhythm. Equal weights within a metric
// tier give the clean 1,2,4,8,16,32 division progression.
static inline uint8_t DivisionWeight(uint8_t step) {
  uint8_t tier;                                  // metric strength, 0 = strongest
  if (step == 0)            tier = 0;            // downbeat        -> 1/32
  else if (step % 16 == 0)  tier = 1;            // half            -> 2/32
  else if (step % 8 == 0)   tier = 2;            // quarters        -> 4/32
  else if (step % 4 == 0)   tier = 3;            // eighths         -> 8/32
  else if (step % 2 == 0)   tier = 4;            // sixteenths      -> 16/32
  else                      tier = 5;            // off-grid 32nds  -> 32/32
  return 255 - tier * 40;                        // 255,215,175,135,95,55
}

uint8_t PatternGenerator::ReadDrumMap(
    uint8_t step,
    uint8_t instrument,
    uint8_t x,
    uint8_t y) {
  uint8_t i, j;

  if (bank_ == 3) {
    uint8_t rot = ((static_cast<uint16_t>(x) * 16) >> 8)              // Map X: global 0..15
                + instrument * ((static_cast<uint16_t>(y) * 8) >> 8); // Map Y: per-channel spread
    uint8_t s = (step + 32 - rot) & 31;                              // rotate within the 32-step bar
    return DivisionWeight(s);
  }

  if (henri_) {
    i = static_cast<uint8_t>(static_cast<uint16_t>(x) * 3 / 255);
    j = static_cast<uint8_t>(static_cast<uint16_t>(y) * 3 / 255);
    if (i > 3) i = 3;
    if (j > 3) j = 3;
  } else {
    i = x >> 6;
    j = y >> 6;
  }

  const uint8_t* a_map = drum_map[bank_][i][j];
  const uint8_t* b_map = drum_map[bank_][i + 1][j];
  const uint8_t* c_map = drum_map[bank_][i][j + 1];
  const uint8_t* d_map = drum_map[bank_][i + 1][j + 1];

  uint8_t offset = (instrument * kStepsPerPattern) + step;

  uint8_t a = pgm_read_byte(a_map + offset);
  uint8_t b = pgm_read_byte(b_map + offset);
  uint8_t c = pgm_read_byte(c_map + offset);
  uint8_t d = pgm_read_byte(d_map + offset);

  if (henri_) {
    // Match Grids4Live: use global coordinates as blend weights (not local cell fractions)
    uint8_t gx = x >> 1;  // scale 0-255 to 0-127
    uint8_t gy = y >> 1;
    const uint16_t maxVal = 127;
    uint32_t r = ((uint32_t)(a * gx + b * (maxVal - gx)) * gy
                + (uint32_t)(c * gx + d * (maxVal - gx)) * (maxVal - gy))
                / maxVal / maxVal;
    return static_cast<uint8_t>(r > 255 ? 255 : r);
  }

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