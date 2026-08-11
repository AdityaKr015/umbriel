#include "input/keyboard.h"

#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "overview/overview.h"
#include "scene/cheatsheet.h"
#include "server/server.h"
#include "wlr.h"

#include <algorithm>

namespace umbriel {

  Keyboard::Keyboard(Server& server, wlr_input_device* device)
      : m_server(&server), m_keyboard(wlr_keyboard_from_input_device(device)) {
    applyConfig();

    m_modifiers.notify = onModifiers;
    wl_signal_add(&m_keyboard->events.modifiers, &m_modifiers);
    m_key.notify = onKey;
    wl_signal_add(&m_keyboard->events.key, &m_key);
    m_destroy.notify = onDestroy;
    wl_signal_add(&device->events.destroy, &m_destroy);

    wlr_seat_set_keyboard(m_server->seat()->wlr(), m_keyboard);
  }
  void Keyboard::applyConfig() {
    cancelRepeat();
    const Config::Input::Keyboard& configured = config().input.keyboard;
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const xkb_rule_names names{
        .rules = nullptr,
        .model = nullptr,
        .layout = configured.layout.empty() ? nullptr : configured.layout.c_str(),
        .variant = configured.variant.empty() ? nullptr : configured.variant.c_str(),
        .options = nullptr,
    };
    xkb_keymap* keymap =
        context == nullptr ? nullptr : xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (keymap != nullptr) {
      wlr_keyboard_set_keymap(m_keyboard, keymap);
      xkb_keymap_unref(keymap);
    } else {
      wlr_log(WLR_ERROR, "failed to create keyboard keymap");
    }
    if (context != nullptr) {
      xkb_context_unref(context);
    }
    wlr_keyboard_set_repeat_info(m_keyboard, configured.repeatRate, configured.repeatDelay);
  }

  Keyboard::~Keyboard() {
    if (m_repeatTimer != nullptr) {
      wl_event_source_remove(m_repeatTimer);
      m_repeatTimer = nullptr;
    }
    if (m_modifiers.link.next != nullptr) {
      wl_list_remove(&m_modifiers.link);
      wl_list_remove(&m_key.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Keyboard::onModifiers(wl_listener* listener, void* /*data*/) {
    Keyboard* self;
    self = wl_container_of(listener, self, m_modifiers);
    self->handleModifiers();
  }

  void Keyboard::onKey(wl_listener* listener, void* data) {
    Keyboard* self;
    self = wl_container_of(listener, self, m_key);
    self->handleKey(data);
  }

  void Keyboard::onDestroy(wl_listener* listener, void* /*data*/) {
    Keyboard* self;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void Keyboard::handleModifiers() {
    cancelRepeat();
    m_server->notifyIdleActivity();
    wlr_seat* seat = m_server->seat()->wlr();
    wlr_seat_set_keyboard(seat, m_keyboard);
    wlr_seat_keyboard_notify_modifiers(seat, &m_keyboard->modifiers);
    m_server->cursor()->refreshInteractiveCursor();
  }

  void Keyboard::handleKey(void* data) {
    auto* event = static_cast<wlr_keyboard_key_event*>(data);
    m_server->notifyIdleActivity();
    wlr_seat* seat = m_server->seat()->wlr();

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t* syms = nullptr;
    int nsyms = xkb_state_key_get_syms(m_keyboard->xkb_state, keycode, &syms);
    const xkb_keysym_t* rawSyms = nullptr;
    xkb_keymap* keymap = xkb_state_get_keymap(m_keyboard->xkb_state);
    const xkb_layout_index_t layout = xkb_state_key_get_layout(m_keyboard->xkb_state, keycode);
    const int nraw = xkb_keymap_key_get_syms_by_level(keymap, keycode, layout, 0, &rawSyms);
    const uint32_t rawSym = nraw > 0 ? rawSyms[0] : XKB_KEY_NoSymbol;

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(m_keyboard);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      cancelRepeat();
      for (int i = 0; i < nsyms; ++i) {
        handled = m_server->handleVtSwitch(syms[i], modifiers) || handled;
      }
      const Keybind* matched = nullptr;
      for (int i = 0; i < nsyms; ++i) {
        const Keybind* result = m_server->handleKeybind(syms[i], rawSym, modifiers);
        if (result != nullptr) {
          matched = result;
          handled = true;
          break;
        }
      }
      if (matched != nullptr) {
        armRepeat(*matched, event->keycode);
      }
      // Unbound plain keys drive overview navigation instead of reaching
      // clients, unless a layer surface (launcher, panel) holds the keyboard:
      // its Escape/arrows belong to it, not to the filmstrip.
      if (!handled && seat->keyboard_state.focused_surface == nullptr) {
        Overview* overview = m_server->overview();
        const uint32_t plain = modifiers & ~(WLR_MODIFIER_CAPS | WLR_MODIFIER_MOD2);
        if (overview != nullptr && overview->interactive() && plain == 0) {
          for (int i = 0; i < nsyms && !handled; ++i) {
            handled = overview->handleFallbackKey(syms[i]);
          }
        }
      }
      // Any non-modifier key press dismisses the cheatsheet, except the key
      // that just toggled it.
      if (Cheatsheet* sheet = m_server->cheatsheet(); sheet != nullptr && sheet->visible()) {
        const bool cheatsheetBind = matched != nullptr
            && (matched->action == KeybindAction::CheatsheetToggle
                || matched->action == KeybindAction::CheatsheetOpen
                || matched->action == KeybindAction::CheatsheetClose);
        const bool modifierOnly = nsyms > 0 && syms[0] >= XKB_KEY_Shift_L && syms[0] <= XKB_KEY_Hyper_R;
        if (!cheatsheetBind && !modifierOnly) {
          sheet->hide();
        }
      }
    } else if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED) {
      if (m_repeatArmed && event->keycode == m_repeatKeycode) {
        cancelRepeat();
      }
    }

    if (!handled) {
      // Overview holds the seat, so windows see no keys until it closes. It
      // never hands keyboard focus to a view while open (focusView skips the
      // seat enter), so a non-null focus here is a layer surface that took it
      // deliberately, e.g. a launcher panel; those keep typing.
      if (Overview* overview = m_server->overview();
          overview != nullptr && overview->active() && seat->keyboard_state.focused_surface == nullptr) {
        return;
      }
      wlr_seat_set_keyboard(seat, m_keyboard);
      wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
  }

  void Keyboard::handleDestroy() {
    if (m_repeatTimer != nullptr) {
      wl_event_source_remove(m_repeatTimer);
      m_repeatTimer = nullptr;
    }
    wl_list_remove(&m_modifiers.link);
    wl_list_remove(&m_key.link);
    wl_list_remove(&m_destroy.link);
    m_modifiers.link.next = nullptr;
    m_key.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_server->removeKeyboard(this);
  }

  void Keyboard::armRepeat(const Keybind& bind, uint32_t keycode) {
    const int32_t rate = m_keyboard->repeat_info.rate;
    const int32_t delay = m_keyboard->repeat_info.delay;
    const bool repeatable =
        bind.action != KeybindAction::ScratchpadToggle && bind.action != KeybindAction::ScratchpadFocusNext;
    if (!bind.repeat || !repeatable || rate <= 0 || delay <= 0) {
      cancelRepeat();
      return;
    }
    m_repeatBind = bind;
    m_repeatKeycode = keycode;
    m_repeatIntervalMs = std::max(1, 1000 / rate);
    if (m_repeatTimer == nullptr) {
      wl_event_loop* loop = wl_display_get_event_loop(m_server->display());
      m_repeatTimer = wl_event_loop_add_timer(loop, onRepeatTimer, this);
      if (m_repeatTimer == nullptr) {
        return;
      }
    }
    wl_event_source_timer_update(m_repeatTimer, delay);
    m_repeatArmed = true;
  }

  void Keyboard::cancelRepeat() {
    if (m_repeatArmed) {
      wl_event_source_timer_update(m_repeatTimer, 0);
      m_repeatArmed = false;
    }
  }

  int Keyboard::onRepeatTimer(void* data) {
    auto* self = static_cast<Keyboard*>(data);
    if (!self->m_repeatArmed) {
      return 0;
    }
    if (self->m_server->sessionLocked()) {
      self->cancelRepeat();
      return 0;
    }
    self->m_server->notifyIdleActivity();
    self->m_server->executeKeybindAction(self->m_repeatBind);
    wl_event_source_timer_update(self->m_repeatTimer, self->m_repeatIntervalMs);
    return 0;
  }

} // namespace umbriel
