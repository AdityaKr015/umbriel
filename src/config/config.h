#pragma once

#include <array>
#include <string>
#include <vector>

namespace umbriel {

  struct Config {
    struct Appearance {
      int borderWidth = 2;
      int cornerRadius = 10;
      std::array<float, 4> borderFocused{0.48F, 0.64F, 1.0F, 1.0F};
      std::array<float, 4> borderUnfocused{0.16F, 0.16F, 0.20F, 1.0F};
      int animationMs = 250;
    } appearance;

    struct Layout {
      int gap = 8;
      double defaultWidthFraction = 0.5;
      std::vector<double> widthPresets{1.0 / 3, 0.5, 2.0 / 3};
      int scrollWheelStep = 60;
    } layout;

    struct General {
      std::string terminal;
    } general;
  };

  [[nodiscard]] const Config& config();
  void loadConfig(const char* explicitPath);

} // namespace umbriel
