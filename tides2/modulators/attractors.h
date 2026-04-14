#include "stmlib/stmlib.h"
#include "stmlib/dsp/dsp.h"
#include "tides2/resources.h"
#include "stmlib/utils/random.h"
#include "stmlib/dsp/units.h"

namespace tides {

using namespace std;
using namespace stmlib;

#define DS_DXDT(x,y,z) a * (y - x)
#define DS_DYDT(x,y,z) (c - a) * x - x * z + c * y
#define DS_DZDT(x,y,z) x * y - b * z

const float f_scale = 2.0439497f / 62500.0f;

const float max_f = 0.02f;

enum Speed {
  SLOW,
  MID,
  FAST
};

typedef struct Coordinates {
  float x;
  float y;
  float z;

  void Init() {
    x = Random::GetFloat();
    y = Random::GetFloat();
    z = Random::GetFloat();
  }
} Coordinates;

class Attractors {
  public:

  void Init() {
    t_coordinates_.Init();
    r_coordinates_.Init();
    l_coordinates_.Init();
    c_coordinates_.Init();
    g1_reset_ = false;
    g2_reset_ = false;
  }  

  void Reset(bool g1_reset, bool g2_reset) {
    g1_reset_ = g1_reset;
    g2_reset_ = g2_reset;
  }

  void set_thomas(float thomas) {
    thomas_ = thomas;
  }

  void set_chua(float chua) {
    chua_ = chua;
  }

  void set_lorenz(float lorenz) {
    lorenz_ = lorenz;
  }

  void set_rossler(float rossler) {
    rossler_ = rossler;
  }

  void set_gain(float gain) {
    gain_ = gain;
  }

  void set_speed(int speed) {
    speed_ = Speed(speed);
  }

  float channels(int8_t index) {
    return channels_[index];
  }

  void Process(float frequency, float alt_frequency, int pair) {
    switch (pair)
    {
    case 0:
      {
        if (g1_reset_) l_coordinates_.Init();
        else ProcessLorenzAttractor(frequency);

        if (g2_reset_) r_coordinates_.Init();
        else ProcessRosslerAttractor(alt_frequency);
        
        channels_[0] = lorenz_output(l_coordinates_.x);
        channels_[1] = lorenz_output(l_coordinates_.y);
        channels_[2] = rossler_output(r_coordinates_.x);
        channels_[3] = rossler_output(r_coordinates_.y);
      }
      break;
    case 1:
      {
        if (g1_reset_) t_coordinates_.Init();
        else ProcessThomasSymmetricAttractor(frequency);

        if (g2_reset_) c_coordinates_.Init();
        else ProcessChuaAttractor(alt_frequency);
        
        channels_[0] = thomas_output(t_coordinates_.x);
        channels_[1] = thomas_output(t_coordinates_.y);
        channels_[2] = chua_output(c_coordinates_.x);
        channels_[3] = chua_output(c_coordinates_.y);
      }
      break;
    default:
      break;
    }
  }

  void ProcessLorenzAttractor(float f) {
    float frequency = 1.3f * f;

    switch (speed_) {
        case SLOW:
        frequency /= (8.0f * 8.0f);
        break;
        case FAST:
        break;
        default:
        frequency /= 8.0f;
        break;
    }
    CONSTRAIN(frequency, 0.0f, max_f);

    const float sigma = 10.0f;
    const float rho = 28.0f * (1.0f + (lorenz_ / 2.0f));
    const float beta = 8.0f / 3.0f;

    float x = l_coordinates_.x;
    float y = l_coordinates_.y;
    float z = l_coordinates_.z;

    x += (frequency * ((sigma * (l_coordinates_.y - l_coordinates_.x))));
    y += (frequency * ((l_coordinates_.x * (rho - l_coordinates_.z)) - l_coordinates_.y));
    z += (frequency * ((l_coordinates_.x * l_coordinates_.y) - (beta * l_coordinates_.z)));

    l_coordinates_.x = x;
    l_coordinates_.y = y;
    l_coordinates_.z = z;
  }

  void ProcessRosslerAttractor(float f) {

    float frequency = 1.3f * f;

    switch (speed_) {
        case SLOW:
        frequency /= 8.0f;
        break;
        case FAST:
        frequency *= 8.0f;
        break;
        default:
        break;
    }
    CONSTRAIN(frequency, 0.0f, max_f);

    const float max_a = 0.35f;
    const float min_a = 0.2f;
    const float a = ((max_a - min_a) * rossler_ + min_a);
    const float b = 0.2;
    const float c = 5.7;

    float x = r_coordinates_.x;
    float y = r_coordinates_.y;
    float z = r_coordinates_.z;

    x += (-r_coordinates_.y - r_coordinates_.z) * frequency;
    y += (r_coordinates_.x + a * r_coordinates_.y) * frequency;
    z += (b + r_coordinates_.z * (r_coordinates_.x - c)) * frequency;

    r_coordinates_.x = x;
    r_coordinates_.y = y;
    r_coordinates_.z = z;
  }

  /*float ProcessHenonAttractor(float f) {

    float frequency = f;
    switch (speed_) {
        case SLOW:
        frequency /= 8.0f;
        break;
        case FAST:
        break;
        default:
        frequency /= 4.0f;
        break;
    }

    CONSTRAIN(frequency, 0.0f, 0.25f);
    frequency *= 32.0f;

    const float max_a = 1.5f;
    const float min_a = 0.05f;
    float a = ((max_a - min_a) * thomas_ + min_a);
    float b = 0.3f;
    CONSTRAIN(a, min_a, max_a);

    const float amp = 0.5f;
    float x = tx_;
    float y = ty_;
    float z = tz_;
    
    float nextX = 1 - a * x * x + y;
    float nextY = b * x;

    x += frequency * dx;
    y += frequency * dy;
    z += frequency * dz;

    tx_ = x;
    ty_ = y;
    tz_ = z;

    return amp * (y - 2.f) * gain_;
  }*/

  void ProcessThomasSymmetricAttractor(float frequency) {
    switch (speed_) {
        case SLOW:
        frequency /= 8.0f;
        break;
        case FAST:
        break;
        default:
        frequency /= 4.0f;
        break;
    }

    CONSTRAIN(frequency, 0.0f, 0.25f);
    frequency *= 32.0f;

    const float max_b = 0.2f;
    const float min_b = 0.02f;
    float b = ((max_b - min_b) * thomas_ + min_b);
    CONSTRAIN(b, min_b, max_b);

    float x = t_coordinates_.x;
    float y = t_coordinates_.y;
    float z = t_coordinates_.z;
    
    const float dx = tcsa(y, x, b);
    const float dy = tcsa(z, y, b);
    const float dz = tcsa(x, z, b);
    x += frequency * dx;
    y += frequency * dy;
    z += frequency * dz;

    t_coordinates_.x = x;
    t_coordinates_.y = y;
    t_coordinates_.z = z;
  }

  void ProcessChuaAttractor(float f) {

    float frequency = 1.3f * f;
    switch (speed_) {
        case SLOW:
        frequency /= (8.0f * 8.0f);
        break;
        case FAST:
        break;
        default:
        frequency /= 16.0f;
        break;
    }
    CONSTRAIN(frequency, 0.0f, 0.01f);
    const float a = 42.0f;
    const float max_b = 6.0f;
    const float min_b = 1.0f;
    const float b = ((max_b - min_b) * chua_ + min_b);
    const float c = 28.0f;

    float x = c_coordinates_.x;
    float y = c_coordinates_.y;
    float z = c_coordinates_.z;
    
    const float dx = DS_DXDT(x, y, z);
    const float dy = DS_DYDT(x, y, z);
    const float dz = DS_DZDT(x, y, z);
    x += frequency * dx;
    y += frequency * dy;
    z += frequency * dz;
    
    c_coordinates_.x = x;
    c_coordinates_.y = y;
    c_coordinates_.z = z;
  }

  private:
    bool g1_reset_;
    bool g2_reset_;
    float thomas_;
    float gain_;
    float rossler_;
    float lorenz_;
    float chua_;   
    Speed speed_;
    float channels_[4];
    Coordinates t_coordinates_;
    Coordinates r_coordinates_;
    Coordinates l_coordinates_;
    Coordinates c_coordinates_;

    inline float chua_output(float in) {
      const float offset = -0.5f;
      const float amp = 10.0f / 8.0f;
      float output = (in + 18.0f) / 36.0f;

      return (amp * output + offset) * 10.0f * gain_;
    }

    inline float thomas_output(float in) {
      return 0.5f * (in - 2.f) * gain_;
    }

    inline float rossler_output(float in) {
      return (in / 2.0f) * gain_;
    }

    inline float lorenz_output(float in) {
      return ((1.5f * in) / 4.0f) * gain_;
    }

    inline float tcsa(float  v, const float w, const float b) {
        v *= 0.159155f; // Convert radians to phase.
        // need to calc wrap here since InterpolateWrap can't handle negatives
        // Using floorf is too slow... The ternary is apparently faster
        v -= static_cast<float>(static_cast<int32_t>(v));
        v = v < 0.0f ? 1.0f - v : v;
        return Interpolate(lut_sine, v, 1024.0f) - b * w;
    }
};

}