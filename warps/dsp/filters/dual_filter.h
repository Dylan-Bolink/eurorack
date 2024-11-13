//#pragma once
#ifndef WARPS_DSP_DUAL_FILTER_H_
#define WARPS_DSP_DUAL_FILTER_H_

#include <stdint.h>
#include <math.h>
#include "stmlib/dsp/filter.h"
#include "stmlib/dsp/units.h"

using namespace stmlib;

namespace warps
{

typedef enum FilterLayout {
    LP_HP,
    LP_BP,
    BP_HP,
    BP_BP,
    LP,
    HP,
    BP,
    N
} FilterLayout;

typedef enum FilterConfig {
    DUAL_FILTER,
    STEREO_FILTER
} FilterConfig;

class DualFilter
{
  public:
    DualFilter() {}
    
    void Init() {
        filterL_.Init();
        filterR_.Init();
        layout_ = LP_HP;
        config_ = DUAL_FILTER;
        float a0 = (440.0f / 8.0f) / 96000.0f;
        f0 = a0 * 0.25f * SemitonesToRatio(12.0f);
    }

    float* Process(float inL, float inR) {
        static float out[2];
        float o1 = 0.0f;
        switch (layout_) {
            case LP_HP:
                {
                    o1 = filterL_.Process<FILTER_MODE_LOW_PASS>(inL);
                    out[0] = filterL_.Process<FILTER_MODE_LOW_PASS>(o1);
                    out[1] = filterR_.Process<FILTER_MODE_HIGH_PASS>(inR * 0.35f);
                }
                break;
            case LP_BP:
                {
                    o1 = filterL_.Process<FILTER_MODE_LOW_PASS>(inL);
                    out[0] = filterL_.Process<FILTER_MODE_LOW_PASS>(o1);
                    out[1] = filterR_.Process<FILTER_MODE_BAND_PASS>(inR);
                }
                break;
            case BP_HP:
                {
                    out[0] = filterL_.Process<FILTER_MODE_BAND_PASS>(inL);
                    out[1] = filterR_.Process<FILTER_MODE_HIGH_PASS>(inR * 0.35f);
                }
                break;
            case BP_BP:
                {
                    out[0] = filterL_.Process<FILTER_MODE_BAND_PASS>(inL);
                    out[1] = filterR_.Process<FILTER_MODE_BAND_PASS>(inR);
                }
                break;
            case LP:
                {
                    out[0] = filterL_.Process<FILTER_MODE_LOW_PASS>(inL);
                    out[1] = filterR_.Process<FILTER_MODE_LOW_PASS>(inR);
                }
                break;
            case HP:
                {
                    out[0] = filterL_.Process<FILTER_MODE_HIGH_PASS>(inL);
                    out[1] = filterR_.Process<FILTER_MODE_HIGH_PASS>(inR);
                }
                break;
            case BP:
                {
                    out[0] = filterL_.Process<FILTER_MODE_BAND_PASS>(inL);
                    out[1] = filterR_.Process<FILTER_MODE_BAND_PASS>(inR);
                }
                break;
            case N:
                {
                    out[0] = inL - filterL_.Process<FILTER_MODE_BAND_PASS_NORMALIZED>(inL);
                    out[1] = inR - filterR_.Process<FILTER_MODE_BAND_PASS_NORMALIZED>(inR);
                }
                break;
            default:
                break;
        }
        
        return out;
    }

    void SetFreqsRes(float fl, float rl, float fr, float rr) {
        float l_cutoff = 2.2f * f0 * SemitonesToRatio(120.0f * fl);
        float r_cutoff = 2.2f * f0 * SemitonesToRatio(120.0f * fr);
        
        filterL_.set_f_q<FREQUENCY_DIRTY>(l_cutoff, resonance(rl) + 0.125f);
        filterR_.set_f_q<FREQUENCY_DIRTY>(r_cutoff, resonance(rr) + 0.125f);
    }

    void SetLayout(FilterLayout value) {
        layout_ = value;
    }

    void SetLayout(int16_t value) {
        int16_t i = config_ == DUAL_FILTER ? 0 : 4;
        int16_t result = value + i;
        layout_ = static_cast<FilterLayout>(result > 7 ? 7 : result);
    }

    void SetConfig(FilterConfig config) {
        config_ = config;
    }

  private:
    Svf filterL_;
    Svf filterR_;
    FilterLayout layout_;
    FilterConfig config_;
    float f0;

    float resonance(float r) {
        //float resonance = 1.333f * (r - 0.25f);
        float resonance_sqr = exp_amp(r);//r * r * r;
        return resonance_sqr * 48.0f;
    }

    float exp_amp(float x) {
      // Clamp x to the range [0, 1]
      x = fminf(fmaxf(x, 0.0f), 1.0f);
      // Compute the amplification using an exponential function
      return (expf(3.0f * (x - 0.75f)) / 2) - 0.05f;
  }
};

} // namespace warps
#endif
//#endif