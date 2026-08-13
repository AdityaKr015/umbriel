#pragma once

#include <cstdint>

namespace umbriel {

  class Output;

  enum class Easing { Linear, EaseOutCubic, EaseInOutCubic };

  // Owners tick in phase order, and the order is load-bearing. Finishing an
  // overview animation calls Server::focusView, which moves the focused view to
  // the front of the view registry; views must therefore be done with their pass
  // before any overlay runs. Within a phase the order does not matter.
  enum class AnimationPhase : uint8_t {
    Views,
    Workspaces,
    Overlays,
  };

  // Anything the central frame tick advances. Registering with the Server is the
  // only thing an owner has to do; the three traversals (advance, is-anything-
  // running, is-anything-running-for-this-output) all derive from this.
  class Animatable {
  public:
    Animatable() = default;
    virtual ~Animatable() = default;
    Animatable(const Animatable&) = delete;
    Animatable& operator=(const Animatable&) = delete;

    [[nodiscard]] virtual AnimationPhase animationPhase() const = 0;
    // Advance to `nowMsec`; true while anything is still running.
    virtual bool tickAnimations(uint64_t nowMsec) = 0;
    [[nodiscard]] virtual bool hasActiveAnimations() const = 0;
    // Whether `output` has to keep scheduling frames for this owner. An owner
    // spanning every output (the overview) answers true for all of them; one
    // with no output yet answers false for all.
    [[nodiscard]] virtual bool animatesOn(const Output* output) const = 0;
  };

  // A single animatable scalar owned by the animated object. The owner ticks it
  // from the central Server tick and reads current() to drive its scene state.
  // Retargeting mid-flight restarts the curve from the current value, so there
  // is no cancel bookkeeping and no snapping when a target changes.
  class AnimatedValue {
  public:
    // current = target = value; stops animating.
    void snap(double value);
    // Always animates, even when `to` equals current(), so completion is always
    // observable via a final tick. Restarts from current with the full duration.
    void retarget(double to, int durationMs, Easing easing = Easing::EaseOutCubic);
    // Advances the value. Returns true when the value was animating at entry
    // (i.e. the owner must apply current()). The call that reaches the target
    // returns true and leaves animating() false, so owners detect completion as
    // (tick(now) && !animating()).
    bool tick(uint64_t nowMsec);
    [[nodiscard]] double current() const { return m_current; }
    [[nodiscard]] double target() const { return m_target; }
    [[nodiscard]] bool animating() const { return m_animating; }

  private:
    double m_from = 0;
    double m_target = 0;
    double m_current = 0;
    uint64_t m_startMsec = 0; // 0 = clock starts on the first tick
    uint64_t m_durationMsec = 1;
    Easing m_easing = Easing::EaseOutCubic;
    bool m_animating = false;
  };

} // namespace umbriel
