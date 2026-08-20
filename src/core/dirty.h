#pragma once

#include <cstdint>

namespace umbriel {

  // Work that has to happen before the next frame, recorded rather than done.
  //
  // These used to be called directly at each site that invalidated something,
  // which had two costs. The same three or four calls were repeated at every such
  // site and drifted apart as one was updated and another forgotten; and a state
  // change reached from several paths in one frame re-did the work once per path.
  //
  // Recording instead means each site says what became stale, and the frame
  // handler decides once, in a fixed order, what to rebuild.
  enum class Dirty : uint32_t {
    None = 0,
    // Layer-shell surfaces set exclusive zones, which is what defines the usable
    // area. Everything below depends on the result, so this runs first.
    LayerArrange = 1U << 0,
    // At least one workspace on this output owes an arrange. Which ones, and
    // whether each animates, is recorded on the workspaces themselves (hidden
    // workspaces are arranged too, so it is not one piece of state per output).
    // This bit is the part that belongs here: where the work sits in the order.
    Layout = 1U << 1,
    // Chrome that sits over the layout: the config-error banner, the desktop
    // backdrop, the keybind cheatsheet, the session-quit confirmation.
    Banner = 1U << 2,
    Backdrop = 1U << 3,
    Cheatsheet = 1U << 4,
    QuitConfirm = 1U << 5,
  };

  // Deliberately not here: the session-lock blanking rect. Everything above can
  // be a frame late without consequence; that cannot. It covers the desktop while
  // the session is locked, so deferring it past a mode or output change would
  // show a sliver of what the lock exists to hide. It stays an immediate call.

  [[nodiscard]] constexpr Dirty operator|(Dirty a, Dirty b) {
    return static_cast<Dirty>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
  }

  [[nodiscard]] constexpr Dirty operator&(Dirty a, Dirty b) {
    return static_cast<Dirty>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
  }

  constexpr Dirty& operator|=(Dirty& a, Dirty b) { return a = a | b; }

  [[nodiscard]] constexpr bool any(Dirty set) { return static_cast<uint32_t>(set) != 0; }
  [[nodiscard]] constexpr bool has(Dirty set, Dirty bit) { return any(set & bit); }

} // namespace umbriel
