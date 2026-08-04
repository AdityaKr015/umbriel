#include "input/keyboard.h"

#include "config/config.h"
#include "input/cursor.h"
#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"

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
    if (m_modifiers.link.next != nullptr) {
      wl_list_remove(&m_modifiers.link);
      wl_list_remove(&m_key.link);
      wl_list_remove(&m_destroy.link);
    }
  }

  void Keyboard::onModifiers(wl_listener* listener, void* /*data*/) {
    Keyboard* self = wl_container_of(listener, self, m_modifiers);
    self->handleModifiers();
  }

  void Keyboard::onKey(wl_listener* listener, void* data) {
    Keyboard* self = wl_container_of(listener, self, m_key);
    self->handleKey(data);
  }

  void Keyboard::onDestroy(wl_listener* listener, void* /*data*/) {
    Keyboard* self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void Keyboard::handleModifiers() {
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
      for (int i = 0; i < nsyms; ++i) {
        handled = m_server->handleVtSwitch(syms[i], modifiers) || handled;
      }
      for (int i = 0; i < nsyms; ++i) {
        handled = m_server->handleKeybind(syms[i], rawSym, modifiers) || handled;
      }
    }

    if (!handled) {
      wlr_seat_set_keyboard(seat, m_keyboard);
      wlr_seat_keyboard_notify_key(seat, event->time_msec, event->keycode, event->state);
    }
  }

  void Keyboard::handleDestroy() {
    wl_list_remove(&m_modifiers.link);
    wl_list_remove(&m_key.link);
    wl_list_remove(&m_destroy.link);
    m_modifiers.link.next = nullptr;
    m_key.link.next = nullptr;
    m_destroy.link.next = nullptr;
    m_server->removeKeyboard(this);
  }

} // namespace umbriel
