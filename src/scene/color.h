#pragma once
#include <array>

namespace umbriel {

  // Premultiplied RGBA from straight-alpha base × opacity multiplier.
  inline void premultiplied(float out[4], const std::array<float, 4>& base, float opacity) {
    const float a = base[3] * opacity;
    out[0] = base[0] * a;
    out[1] = base[1] * a;
    out[2] = base[2] * a;
    out[3] = a;
  }

} // namespace umbriel
