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
// CV reader.

#ifndef TIDES_CV_READER_H_
#define TIDES_CV_READER_H_

#include "stmlib/stmlib.h"
#include "stmlib/dsp/hysteresis_quantizer.h"

#include "tides2/drivers/cv_adc.h"
#include "tides2/drivers/pots_adc.h"
#include "tides2/cv_reader_channel.h"
#include "tides2/settings.h"

namespace tides {

class CvReader {
 public:
  CvReader() { }
  ~CvReader() { }
  
  void Init(Settings* settings);
  void Read(IOBuffer::Block* block);
  
  const CvReaderChannel& channel(int index) const {
    return cv_reader_channel_[index];
  }
  
  inline bool fm_cv_thresholded() const {
    return cv_adc_.float_value(CV_ADC_CHANNEL_FM) < -0.17f;
  }

  void    SetFrequencyLocked(bool locked);
  void    SetLockMode(uint8_t mode);
  bool    frequency_locked()   const { return frequency_locked_; }
  bool    clock_patched()      const { return clock_patched_; }
  bool    lock_active()        const { return frequency_locked_ && !clock_patched_; }
  float   lock_reference_pot() const { return lock_reference_pot_; }
  uint8_t lock_mode()          const { return lock_mode_; }
  
  inline float CenterDetent(float x) const {
    if (x < 0.49f) {
      x *= 1.02040816f;
    } else if (x > 0.51f) {
      x -= 0.02f;
      x *= 1.02040816f;
    } else {
      x = 0.5f;
    }
    return x;
  }

 private:
  Settings* settings_;
  CvAdc cv_adc_;
  PotsAdc pots_adc_;
  float note_lp_;
  float alt_note_lp_;
  bool  frequency_locked_;
  bool  clock_patched_;             // last observed CLOCK gate patched state
  float locked_note_semitones_;     // locked pitch in semitones (post-CenterDetent, exact)
  float lock_reference_pot_;        // raw pot position at lock time (for delta calc)
  float previous_pot_value_;
  uint8_t lock_mode_;               // 0=semitones, 1=fifths+oct, 2=octaves
  stmlib::HysteresisQuantizer octave_quantizer_;

  CvReaderChannel cv_reader_channel_[kNumParameters];
  CvReaderChannel alt_note_channel_;
  
  DISALLOW_COPY_AND_ASSIGN(CvReader);
};

}  // namespace tides

#endif  // TIDES_CV_READER_H_
