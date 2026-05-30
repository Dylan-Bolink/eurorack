// Copyright 2017 Emilie Gillet.
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
// Settings storage.

#ifndef TIDES_SETTINGS_H_
#define TIDES_SETTINGS_H_

#include "stmlib/stmlib.h"
#include "stmlib/system/storage.h"

#include "tides2/io_buffer.h"

namespace tides {

struct DacCalibrationData {
  float scale;
  float offset;
  inline uint16_t code(float level) const {
    int32_t value = level * scale + offset;
    CONSTRAIN(value, 0, 65535);
    return static_cast<uint16_t>(value);
  }
};

struct AdcCalibrationData {
  float scale;
  float offset;
};

struct PersistentData {
  DacCalibrationData dac_calibration[kNumCvOutputs];
  AdcCalibrationData adc_calibration[kNumParameters];
  uint8_t padding[16];
  
  enum { tag = 0x494C4143 };  // CALI
};

struct State {
  uint8_t mode;
  uint8_t range;
  uint8_t output_mode;
  uint8_t color_blind;
  uint8_t frequency_locked;      // 0 = unlocked, 1 = locked (was padding[0])
  uint8_t locked_frequency_hi;
  uint8_t locked_frequency_lo;
  uint8_t frequency_lock_mode;
  uint8_t alt_mode;

  enum { tag = 0x54415453 };  // STAT

  inline void set_locked_frequency(float semitones) {
    float t = (semitones + 96.0f) / 192.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    uint16_t v = static_cast<uint16_t>(t * 65535.0f);
    locked_frequency_hi = static_cast<uint8_t>(v >> 8);
    locked_frequency_lo = static_cast<uint8_t>(v & 0xFF);
  }
  inline float get_locked_frequency() const {
    uint16_t v = (static_cast<uint16_t>(locked_frequency_hi) << 8) | locked_frequency_lo;
    return (v / 65535.0f) * 192.0f - 96.0f;
  }
  inline void set_lock_mode(uint8_t m) { frequency_lock_mode = m; }
  inline uint8_t get_lock_mode() const { return frequency_lock_mode; }
};

class Settings {
 public:
  Settings() { }
  ~Settings() { }
  
  bool Init();

  void SavePersistentData();
  void SaveState();
  
  inline State* mutable_state() {
    return &state_;
  }
  
  inline const State& state() const {
    return state_;
  }

  inline uint16_t dac_code(int index, float level) const {
    return persistent_data_.dac_calibration[index].code(level);
  }
  
  inline const AdcCalibrationData& adc_calibration_data(int index) const {
    return persistent_data_.adc_calibration[index];
  }
  
  inline AdcCalibrationData* mutable_adc_calibration_data(int index) {
    return &persistent_data_.adc_calibration[index];
  }

  inline DacCalibrationData* mutable_dac_calibration_data(int index) {
    return &persistent_data_.dac_calibration[index];
  }

 private:
  PersistentData persistent_data_;
  State state_;
  
  stmlib::ChunkStorage<
      0x08004000,
      0x08008000,
      PersistentData,
      State> chunk_storage_;
  
  DISALLOW_COPY_AND_ASSIGN(Settings);
};

}  // namespace tides

#endif  // TIDES_SETTINGS_H_
