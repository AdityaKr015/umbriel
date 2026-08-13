#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "layer/layer_surface.h"
#include "output/output.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "server/actions.h"
#include "server/server.h"
#include "view/view.h"
#include "wlr.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

#include <string_view>

namespace umbriel {

  bool Server::handleVtSwitch(uint32_t keysym, uint32_t modifiers) {
    if (m_session == nullptr) {
      return false;
    }

    unsigned vt = 0;
    if (keysym >= XKB_KEY_XF86Switch_VT_1 && keysym <= XKB_KEY_XF86Switch_VT_12) {
      vt = 1 + (keysym - XKB_KEY_XF86Switch_VT_1);
    } else if (
        (modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)
        && keysym >= XKB_KEY_F1
        && keysym <= XKB_KEY_F12
    ) {
      vt = 1 + (keysym - XKB_KEY_F1);
    }

    if (vt == 0) {
      return false;
    }

    if (!wlr_session_change_vt(m_session, vt)) {
      wlr_log(WLR_ERROR, "failed to switch to VT %u", vt);
    }
    return true;
  }

  bool Server::executeKeybindAction(const Keybind& bind, std::string* error) {
    if (error != nullptr) {
      error->clear();
    }
    if (bind.action == KeybindAction::None) {
      return false;
    }
    const ActionHandlerFn handler = actionHandlerFor(bind.action);
    if (handler == nullptr) {
      if (error != nullptr) {
        *error = "action has no handler";
      }
      return false;
    }
    return handler(*this, bind, error);
  }

  bool Server::focusNextWindow() {
    View* next = m_registry.rotateToNext([](const View& view) { return view.mapped() && view.onActiveWorkspace(); });
    if (next == nullptr) {
      return false;
    }
    focusView(next, FocusReason::Directional);
    return true;
  }

  const Keybind* Server::handleKeybind(uint32_t keysym, uint32_t rawKeysym, uint32_t modifiers) {
    if (m_sessionLocked) {
      return nullptr;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const uint32_t lowered = xkb_keysym_to_lower(keysym);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        // Allow submap:reset / submap:disable from the default context to
        // always match, so users can define a global emergency exit.
        if (!m_activeSubmaps.empty() && bind.submap.empty() && isSubmapResetBind(bind)) {
          // Fall through to match below.
        } else {
          continue;
        }
      }
      if (bind.wheel != WheelDirection::None || bind.mouseButton != 0) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected || (lowered != bind.keysym && rawKeysym != bind.keysym)) {
        continue;
      }
      return executeKeybindAction(bind) ? &bind : nullptr;
    }

    return nullptr;
  }

  bool Server::handleWheelBind(WheelDirection direction, uint32_t modifiers) {
    if (m_sessionLocked) {
      return false;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      if (bind.wheel != direction) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      return executeKeybindAction(bind);
    }

    return false;
  }

  bool Server::handleMouseBind(uint32_t button, uint32_t modifiers) {
    if (m_sessionLocked) {
      return false;
    }

    const uint32_t effective = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
    const std::string_view currentSubmap = m_activeSubmaps.empty() ? std::string_view{} : m_activeSubmaps.back();

    for (const Keybind& bind : config().keybinds) {
      if (bind.submap != currentSubmap) {
        continue;
      }
      // Non-mouse binds carry 0 here, which never equals a BTN_* code.
      if (bind.mouseButton != button) {
        continue;
      }
      const uint32_t expected = bind.modifiers | (bind.useMod ? modKey() : 0);
      if (effective != expected) {
        continue;
      }
      return executeKeybindAction(bind);
    }

    return false;
  }

  void Server::pushSubmap(const std::string& name) { m_activeSubmaps.push_back(name); }

  void Server::popSubmap() {
    if (!m_activeSubmaps.empty()) {
      m_activeSubmaps.pop_back();
    }
  }

} // namespace umbriel
