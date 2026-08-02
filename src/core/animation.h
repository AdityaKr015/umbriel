#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace umbriel {

  using AnimId = uint64_t;
  inline constexpr int kAnimMs = 250;

  class Animator {
  public:
    AnimId animate(
        double from, double to, int durationMs, std::function<void(double)> onUpdate, std::function<void()> onDone = {}
    );
    void cancel(AnimId id);
    [[nodiscard]] double currentValue(AnimId id) const;
    bool tick(uint64_t nowMsec);
    [[nodiscard]] bool active() const { return !m_animations.empty(); }

  private:
    struct Animation {
      AnimId id = 0;
      double from = 0;
      double to = 0;
      double current = 0;
      uint64_t startMsec = 0;
      uint64_t durationMsec = 1;
      std::function<void(double)> onUpdate;
      std::function<void()> onDone;
    };

    [[nodiscard]] static uint64_t monotonicMsec();

    std::vector<Animation> m_animations;
    AnimId m_nextId = 1;
    uint64_t m_lastTickMsec = 0;
  };

} // namespace umbriel
