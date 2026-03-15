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


#ifdef __AVR__
#include "avrlib/base.h"
#include <avr/pgmspace.h>
#include "avrlib/resources_manager.h"
#else
#include <stdint.h>
typedef uint8_t prog_uint8_t;
typedef uint16_t prog_uint16_t;
typedef uint32_t prog_uint32_t;
typedef char prog_char;
#ifndef PROGMEM
#define PROGMEM
#endif
#endif

namespace grids {

typedef uint8_t ResourceId;

#ifdef __AVR__
extern const prog_char* const string_table[];

extern const prog_uint16_t* const lookup_table_table[];

extern const prog_uint32_t* const lookup_table32_table[];

extern const prog_uint8_t* const node_table[];
#endif

extern const prog_uint32_t lut_res_euclidean[] PROGMEM;
extern const prog_uint32_t lut_res_tempo_phase_increment[] PROGMEM;
extern const prog_uint8_t node_0[] PROGMEM;
extern const prog_uint8_t node_1[] PROGMEM;
extern const prog_uint8_t node_2[] PROGMEM;
extern const prog_uint8_t node_3[] PROGMEM;
extern const prog_uint8_t node_4[] PROGMEM;
extern const prog_uint8_t node_5[] PROGMEM;
extern const prog_uint8_t node_6[] PROGMEM;
extern const prog_uint8_t node_7[] PROGMEM;
extern const prog_uint8_t node_8[] PROGMEM;
extern const prog_uint8_t node_9[] PROGMEM;
extern const prog_uint8_t node_10[] PROGMEM;
extern const prog_uint8_t node_11[] PROGMEM;
extern const prog_uint8_t node_12[] PROGMEM;
extern const prog_uint8_t node_13[] PROGMEM;
extern const prog_uint8_t node_14[] PROGMEM;
extern const prog_uint8_t node_15[] PROGMEM;
extern const prog_uint8_t node_16[] PROGMEM;
extern const prog_uint8_t node_17[] PROGMEM;
extern const prog_uint8_t node_18[] PROGMEM;
extern const prog_uint8_t node_19[] PROGMEM;
extern const prog_uint8_t node_20[] PROGMEM;
extern const prog_uint8_t node_21[] PROGMEM;
extern const prog_uint8_t node_22[] PROGMEM;
extern const prog_uint8_t node_23[] PROGMEM;
extern const prog_uint8_t node_24[] PROGMEM;
extern const prog_uint8_t node_25[] PROGMEM;
extern const prog_uint8_t node_26[] PROGMEM;
extern const prog_uint8_t node_27[] PROGMEM;
extern const prog_uint8_t node_28[] PROGMEM;
extern const prog_uint8_t node_29[] PROGMEM;
extern const prog_uint8_t node_30[] PROGMEM;
extern const prog_uint8_t node_31[] PROGMEM;
extern const prog_uint8_t node_32[] PROGMEM;
extern const prog_uint8_t node_33[] PROGMEM;
extern const prog_uint8_t node_34[] PROGMEM;
extern const prog_uint8_t node_35[] PROGMEM;
extern const prog_uint8_t node_36[] PROGMEM;
extern const prog_uint8_t node_37[] PROGMEM;
extern const prog_uint8_t node_38[] PROGMEM;
extern const prog_uint8_t node_39[] PROGMEM;
extern const prog_uint8_t node_40[] PROGMEM;
extern const prog_uint8_t node_41[] PROGMEM;
extern const prog_uint8_t node_42[] PROGMEM;
extern const prog_uint8_t node_43[] PROGMEM;
extern const prog_uint8_t node_44[] PROGMEM;
extern const prog_uint8_t node_45[] PROGMEM;
extern const prog_uint8_t node_46[] PROGMEM;
extern const prog_uint8_t node_47[] PROGMEM;
extern const prog_uint8_t node_48[] PROGMEM;
extern const prog_uint8_t node_49[] PROGMEM;
extern const prog_uint8_t node_50[] PROGMEM;
extern const prog_uint8_t node_51[] PROGMEM;
extern const prog_uint8_t node_52[] PROGMEM;
extern const prog_uint8_t node_53[] PROGMEM;
extern const prog_uint8_t node_54[] PROGMEM;
extern const prog_uint8_t node_55[] PROGMEM;
extern const prog_uint8_t node_56[] PROGMEM;
extern const prog_uint8_t node_57[] PROGMEM;
extern const prog_uint8_t node_58[] PROGMEM;
extern const prog_uint8_t node_59[] PROGMEM;
extern const prog_uint8_t node_60[] PROGMEM;
extern const prog_uint8_t node_61[] PROGMEM;
extern const prog_uint8_t node_62[] PROGMEM;
extern const prog_uint8_t node_63[] PROGMEM;
extern const prog_uint8_t node_64[] PROGMEM;
extern const prog_uint8_t node_65[] PROGMEM;
extern const prog_uint8_t node_66[] PROGMEM;
extern const prog_uint8_t node_67[] PROGMEM;
extern const prog_uint8_t node_68[] PROGMEM;
extern const prog_uint8_t node_69[] PROGMEM;
extern const prog_uint8_t node_70[] PROGMEM;
extern const prog_uint8_t node_71[] PROGMEM;
extern const prog_uint8_t node_72[] PROGMEM;
extern const prog_uint8_t node_73[] PROGMEM;
extern const prog_uint8_t node_74[] PROGMEM;
#define STR_RES_DUMMY 0  // dummy
#define LUT_RES_EUCLIDEAN 0
#define LUT_RES_EUCLIDEAN_SIZE 1024
#define LUT_RES_TEMPO_PHASE_INCREMENT 1
#define LUT_RES_TEMPO_PHASE_INCREMENT_SIZE 512
#define NODE_0 0
#define NODE_0_SIZE 96
#define NODE_1 1
#define NODE_1_SIZE 96
#define NODE_2 2
#define NODE_2_SIZE 96
#define NODE_3 3
#define NODE_3_SIZE 96
#define NODE_4 4
#define NODE_4_SIZE 96
#define NODE_5 5
#define NODE_5_SIZE 96
#define NODE_6 6
#define NODE_6_SIZE 96
#define NODE_7 7
#define NODE_7_SIZE 96
#define NODE_8 8
#define NODE_8_SIZE 96
#define NODE_9 9
#define NODE_9_SIZE 96
#define NODE_10 10
#define NODE_10_SIZE 96
#define NODE_11 11
#define NODE_11_SIZE 96
#define NODE_12 12
#define NODE_12_SIZE 96
#define NODE_13 13
#define NODE_13_SIZE 96
#define NODE_14 14
#define NODE_14_SIZE 96
#define NODE_15 15
#define NODE_15_SIZE 96
#define NODE_16 16
#define NODE_16_SIZE 96
#define NODE_17 17
#define NODE_17_SIZE 96
#define NODE_18 18
#define NODE_18_SIZE 96
#define NODE_19 19
#define NODE_19_SIZE 96
#define NODE_20 20
#define NODE_20_SIZE 96
#define NODE_21 21
#define NODE_21_SIZE 96
#define NODE_22 22
#define NODE_22_SIZE 96
#define NODE_23 23
#define NODE_23_SIZE 96
#define NODE_24 24
#define NODE_24_SIZE 96
#define NODE_25 25
#define NODE_25_SIZE 96
#define NODE_26 26
#define NODE_26_SIZE 96
#define NODE_27 27
#define NODE_27_SIZE 96
#define NODE_28 28
#define NODE_28_SIZE 96
#define NODE_29 29
#define NODE_29_SIZE 96
#define NODE_30 30
#define NODE_30_SIZE 96
#define NODE_31 31
#define NODE_31_SIZE 96
#define NODE_32 32
#define NODE_32_SIZE 96
#define NODE_33 33
#define NODE_33_SIZE 96
#define NODE_34 34
#define NODE_34_SIZE 96
#define NODE_35 35
#define NODE_35_SIZE 96
#define NODE_36 36
#define NODE_36_SIZE 96
#define NODE_37 37
#define NODE_37_SIZE 96
#define NODE_38 38
#define NODE_38_SIZE 96
#define NODE_39 39
#define NODE_39_SIZE 96
#define NODE_40 40
#define NODE_40_SIZE 96
#define NODE_41 41
#define NODE_41_SIZE 96
#define NODE_42 42
#define NODE_42_SIZE 96
#define NODE_43 43
#define NODE_43_SIZE 96
#define NODE_44 44
#define NODE_44_SIZE 96
#define NODE_45 45
#define NODE_45_SIZE 96
#define NODE_46 46
#define NODE_46_SIZE 96
#define NODE_47 47
#define NODE_47_SIZE 96
#define NODE_48 48
#define NODE_48_SIZE 96
#define NODE_49 49
#define NODE_49_SIZE 96
#define NODE_50 50
#define NODE_50_SIZE 96
#define NODE_51 51
#define NODE_51_SIZE 96
#define NODE_52 52
#define NODE_52_SIZE 96
#define NODE_53 53
#define NODE_53_SIZE 96
#define NODE_54 54
#define NODE_54_SIZE 96
#define NODE_55 55
#define NODE_55_SIZE 96
#define NODE_56 56
#define NODE_56_SIZE 96
#define NODE_57 57
#define NODE_57_SIZE 96
#define NODE_58 58
#define NODE_58_SIZE 96
#define NODE_59 59
#define NODE_59_SIZE 96
#define NODE_60 60
#define NODE_60_SIZE 96
#define NODE_61 61
#define NODE_61_SIZE 96
#define NODE_62 62
#define NODE_62_SIZE 96
#define NODE_63 63
#define NODE_63_SIZE 96
#define NODE_64 64
#define NODE_64_SIZE 96
#define NODE_65 65
#define NODE_65_SIZE 96
#define NODE_66 66
#define NODE_66_SIZE 96
#define NODE_67 67
#define NODE_67_SIZE 96
#define NODE_68 68
#define NODE_68_SIZE 96
#define NODE_69 69
#define NODE_69_SIZE 96
#define NODE_70 70
#define NODE_70_SIZE 96
#define NODE_71 71
#define NODE_71_SIZE 96
#define NODE_72 72
#define NODE_72_SIZE 96
#define NODE_73 73
#define NODE_73_SIZE 96
#define NODE_74 74
#define NODE_74_SIZE 96
#ifdef __AVR__
typedef avrlib::ResourcesManager<
    ResourceId,
    avrlib::ResourcesTables<
        string_table,
        lookup_table_table> > ResourcesManager;
#endif

}  // namespace grids

#endif  // GRIDS_RESOURCES_H_
