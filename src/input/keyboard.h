#pragma once

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

  private:
    static void onModifiers(wl_listener* listener, void* data);
    static void onKey(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleModifiers();
    void handleKey(void* data);
    void handleDestroy();

    Server* m_server = nullptr;
    wlr_keyboard* m_keyboard = nullptr;

    wl_listener m_modifiers{};
    wl_listener m_key{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
