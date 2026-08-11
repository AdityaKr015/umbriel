#include "core/animation.h"

#include <algorithm>
#include <cmath>

namespace umbriel {

  namespace {
    double applyEasing(Easing easing, double linear) {
      switch (easing) {
      case Easing::Linear:
        return linear;
      case Easing::EaseOutCubic: {
        const double remaining = 1.0 - linear;
        return 1.0 - remaining * remaining * remaining;
      }
      case Easing::EaseInOutCubic:
        return linear < 0.5 ? 4.0 * linear * linear * linear : 1.0 - std::pow(-2.0 * linear + 2.0, 3) / 2.0;
      }
      return linear;
    }
  } // namespace

  void AnimatedValue::snap(double value) {
    m_from = value;
    m_target = value;
    m_current = value;
    m_startMsec = 0;
    m_animating = false;
  }

  void AnimatedValue::retarget(double to, int durationMs, Easing easing) {
    m_from = m_current;
    m_target = to;
    m_durationMsec = static_cast<uint64_t>(std::max(1, durationMs));
    m_easing = easing;
    // The clock starts on the first tick, not here: an animation created while
    // the compositor is idle would otherwise burn its whole duration before the
    // first frame arrives and visibly snap to the end.
    m_startMsec = 0;
    m_animating = true;
  }

  bool AnimatedValue::tick(uint64_t nowMsec) {
    if (!m_animating) {
      return false;
    }
    if (m_startMsec == 0) {
      m_startMsec = nowMsec;
    }
    const double linear = std::clamp(
        static_cast<double>(nowMsec - std::min(nowMsec, m_startMsec)) / static_cast<double>(m_durationMsec), 0.0, 1.0
    );
    if (linear >= 1.0) {
      m_current = m_target;
      m_animating = false;
      return true;
    }
    m_current = m_from + (m_target - m_from) * applyEasing(m_easing, linear);
    return true;
  }

} // namespace umbriel
