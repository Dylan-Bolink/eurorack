// Copyright 2012 Emilie Gillet.
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
// Resources definitions.
//
// Automatically generated with:
// make resources


#ifndef GRIDS_RESOURCES_H_
#define GRIDS_RESOURCES_H_

#include "stmlib/stmlib.h"
#include <stdint.h>

// --- STM32 MACROS ---
#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#endif

#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const uint32_t*)(addr))
#endif

typedef uint8_t  prog_uint8_t;
typedef uint16_t prog_uint16_t;
typedef uint32_t prog_uint32_t;
typedef int8_t   prog_int8_t;
typedef char     prog_char;

namespace grids {

// We only keep the Drum Nodes because we deleted Euclidean/UI modes
extern const uint8_t node_0[];
extern const uint8_t node_1[];
extern const uint8_t node_2[];
extern const uint8_t node_3[];
extern const uint8_t node_4[];
extern const uint8_t node_5[];
extern const uint8_t node_6[];
extern const uint8_t node_7[];
extern const uint8_t node_8[];
extern const uint8_t node_9[];
extern const uint8_t node_10[];
extern const uint8_t node_11[];
extern const uint8_t node_12[];
extern const uint8_t node_13[];
extern const uint8_t node_14[];
extern const uint8_t node_15[];
extern const uint8_t node_16[];
extern const uint8_t node_17[];
extern const uint8_t node_18[];
extern const uint8_t node_19[];
extern const uint8_t node_20[];
extern const uint8_t node_21[];
extern const uint8_t node_22[];
extern const uint8_t node_23[];
extern const uint8_t node_24[];

}  // namespace grids

#endif  // GRIDS_RESOURCES_H_