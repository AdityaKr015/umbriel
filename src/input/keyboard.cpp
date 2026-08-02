#include "input/keyboard.h"

#include "input/seat.h"
#include "server/server.h"
#include "wlr.h"

namespace umbriel {

  Keyboard::Keyboard(Server& server, wlr_input_device* device)
      : m_server(&server), m_keyboard(wlr_keyboard_from_input_device(device)) {
    xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(m_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    wlr_keyboard_set_repeat_info(m_keyboard, 25, 600);

    m_modifiers.notify = onModifiers;
    wl_signal_add(&m_keyboard->events.modifiers, &m_modifiers);
    m_key.notify = onKey;
    wl_signal_add(&m_keyboard->events.key, &m_key);
    m_destroy.notify = onDestroy;
    wl_signal_add(&device->events.destroy, &m_destroy);

    wlr_seat_set_keyboard(m_server->seat()->wlr(), m_keyboard);
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
  }

  void Keyboard::handleKey(void* data) {
    auto* event = static_cast<wlr_keyboard_key_event*>(data);
    m_server->notifyIdleActivity();
    wlr_seat* seat = m_server->seat()->wlr();

    uint32_t keycode = event->keycode + 8;
    const xkb_keysym_t* syms = nullptr;
    int nsyms = xkb_state_key_get_syms(m_keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(m_keyboard);
    if ((modifiers & m_server->modKey()) != 0 && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      for (int i = 0; i < nsyms; ++i) {
        handled = m_server->handleKeybind(syms[i], modifiers, event->keycode) || handled;
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
