#pragma once

#include "config/config.h"

#include <wayland-server-core.h>

struct wlr_input_device;
struct wlr_keyboard;

namespace umbriel {

  class Server;

  class Keyboard {
  public:
    Keyboard(Server& server, wlr_input_device* device);
    ~Keyboard();

    Keyboard(const Keyboard&) = delete;
    Keyboard& operator=(const Keyboard&) = delete;

    [[nodiscard]] wlr_keyboard* wlr() const { return m_keyboard; }
    void applyConfig();

  private:
    static void onModifiers(wl_listener* listener, void* data);
    static void onKey(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleModifiers();
    void handleKey(void* data);
    void handleDestroy();
    void armRepeat(const Keybind& bind, uint32_t keycode);
    void cancelRepeat();
    static int onRepeatTimer(void* data);

    Server* m_server = nullptr;
    wlr_keyboard* m_keyboard = nullptr;

    wl_listener m_modifiers{};
    wl_listener m_key{};
    wl_listener m_destroy{};

    wl_event_source* m_repeatTimer = nullptr;
    Keybind m_repeatBind{};
    uint32_t m_repeatKeycode = 0;
    int m_repeatIntervalMs = 0;
    bool m_repeatArmed = false;
  };

} // namespace umbriel
