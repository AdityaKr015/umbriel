#pragma once

#include <wayland-server-core.h>

struct wlr_seat;

namespace umbriel {

class Server;

class Seat {
public:
  explicit Seat(Server& server);
  ~Seat();

  Seat(const Seat&) = delete;
  Seat& operator=(const Seat&) = delete;

  [[nodiscard]] wlr_seat* wlr() const { return m_seat; }

  void updateCapabilities(bool hasKeyboard);

private:
  static void onRequestCursor(wl_listener* listener, void* data);
  static void onPointerFocusChange(wl_listener* listener, void* data);
  static void onRequestSetSelection(wl_listener* listener, void* data);

  void handleRequestCursor(void* data);
  void handlePointerFocusChange(void* data);
  void handleRequestSetSelection(void* data);

  Server* m_server = nullptr;
  wlr_seat* m_seat = nullptr;

  wl_listener m_requestCursor{};
  wl_listener m_pointerFocusChange{};
  wl_listener m_requestSetSelection{};
};

} // namespace umbriel
