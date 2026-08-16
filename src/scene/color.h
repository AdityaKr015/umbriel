#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>

namespace umbriel {

  // Premultiplied RGBA from straight-alpha base × opacity multiplier.
  inline void premultiplied(float out[4], const std::array<float, 4>& base, float opacity) {
    const float a = base[3] * opacity;
    out[0] = base[0] * a;
    out[1] = base[1] * a;
    out[2] = base[2] * a;
    out[3] = a;
  }

  inline std::string rgbaHex(const std::array<float, 4>& color) {
    const auto byte = [](float component) {
      return static_cast<int>(std::lround(std::clamp(component, 0.0F, 1.0F) * 255.0F));
    };
    return std::format("#{:02X}{:02X}{:02X}{:02X}", byte(color[0]), byte(color[1]), byte(color[2]), byte(color[3]));
  }

} // namespace umbriel
