#include "core/animation.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <ranges>
#include <utility>

namespace umbriel {

  uint64_t Animator::monotonicMsec() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000 + static_cast<uint64_t>(now.tv_nsec) / 1'000'000;
  }

  AnimId Animator::animate(
      double from, double to, int durationMs, Easing easing, std::function<void(double)> onUpdate,
      std::function<void()> onDone
  ) {
    AnimId id = m_nextId++;
    if (id == 0) {
      id = m_nextId++;
    }
    m_animations.push_back({
        .id = id,
        .from = from,
        .to = to,
        .current = from,
        .startMsec = monotonicMsec(),
        .durationMsec = static_cast<uint64_t>(std::max(1, durationMs)),
        .easing = easing,
        .onUpdate = std::move(onUpdate),
        .onDone = std::move(onDone),
    });
    return id;
  }

  void Animator::cancel(AnimId id) {
    if (id == 0) {
      return;
    }
    std::erase_if(m_animations, [id](const Animation& animation) { return animation.id == id; });
  }

  double Animator::currentValue(AnimId id) const {
    const auto it = std::ranges::find_if(m_animations, [id](const Animation& animation) { return animation.id == id; });
    return it == m_animations.end() ? 0.0 : it->current;
  }

  bool Animator::tick(uint64_t nowMsec) {
    if (nowMsec == m_lastTickMsec) {
      return active();
    }
    m_lastTickMsec = nowMsec;

    std::vector<AnimId> ids;
    ids.reserve(m_animations.size());
    for (const Animation& animation : m_animations) {
      ids.push_back(animation.id);
    }

    for (AnimId id : ids) {
      auto it = std::ranges::find_if(m_animations, [id](const Animation& animation) { return animation.id == id; });
      if (it == m_animations.end()) {
        continue;
      }
      const double linear = std::clamp(
          static_cast<double>(nowMsec - std::min(nowMsec, it->startMsec)) / static_cast<double>(it->durationMsec), 0.0,
          1.0
      );
      double eased = 0.0;
      switch (it->easing) {
      case Easing::Linear:
        eased = linear;
        break;
      case Easing::EaseOutCubic: {
        const double remaining = 1.0 - linear;
        eased = 1.0 - remaining * remaining * remaining;
        break;
      }
      case Easing::EaseInOutCubic:
        eased = linear < 0.5 ? 4.0 * linear * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 3) / 2.0;
        break;
      }
      it->current = it->from + (it->to - it->from) * eased;
      const double value = it->current;
      const bool done = linear >= 1.0;
      const auto update = it->onUpdate;
      update(value);

      it = std::ranges::find_if(m_animations, [id](const Animation& animation) { return animation.id == id; });
      if (!done || it == m_animations.end()) {
        continue;
      }
      auto onDone = std::move(it->onDone);
      m_animations.erase(it);
      if (onDone) {
        onDone();
      }
    }
    return active();
  }

} // namespace umbriel
