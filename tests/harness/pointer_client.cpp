// Drives a virtual pointer against a running compositor, for the harness.
//
// The headless backend has no input devices (wlroots 0.20 dropped
// wlr_headless_add_input_device), so this is the only way to exercise pointer
// hit-testing without a physical mouse: bind zwp_virtual_pointer_manager_v1,
// create a pointer, and feed it absolute motion and button events. The
// compositor attaches it to its wlr_cursor like any other pointer, so these
// events run the same path a real mouse does.
//
// Usage: pointer-client <width> <height> <command>...
//   move <x> <y>        absolute motion within the given extent
//   click <button>      press and release (button is an evdev BTN_* code)
//   press <button>
//   release <button>
//
// Commands run in order, each followed by a frame and a roundtrip so the
// compositor has processed one before the next is sent.

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <string>
#include <vector>
#include <wayland-client.h>

namespace {

  struct State {
    wl_seat* seat = nullptr;
    zwlr_virtual_pointer_manager_v1* manager = nullptr;
  };

  void handleGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto* state = static_cast<State*>(data);
    if (std::strcmp(interface, wl_seat_interface.name) == 0) {
      state->seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    } else if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
      state->manager = static_cast<zwlr_virtual_pointer_manager_v1*>(
          wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, version < 2 ? version : 2)
      );
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };

  // The compositor only reads time_msec for event ordering, so a monotonically
  // increasing counter is enough and keeps runs reproducible.
  uint32_t nextTime() {
    static uint32_t time = 1000;
    time += 10;
    return time;
  }

} // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::println(stderr, "usage: {} <width> <height> <command>...", argv[0]);
    return EXIT_FAILURE;
  }
  const auto extentWidth = static_cast<uint32_t>(std::atoi(argv[1]));
  const auto extentHeight = static_cast<uint32_t>(std::atoi(argv[2]));

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::println(stderr, "pointer-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  State state;
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(display);

  if (state.manager == nullptr) {
    std::println(stderr, "pointer-client: compositor does not offer zwlr_virtual_pointer_manager_v1");
    return EXIT_FAILURE;
  }

  zwlr_virtual_pointer_v1* pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(state.manager, state.seat);
  if (pointer == nullptr) {
    std::println(stderr, "pointer-client: failed to create a virtual pointer");
    return EXIT_FAILURE;
  }

  const std::vector<std::string> args(argv + 3, argv + argc);
  for (size_t i = 0; i < args.size(); ++i) {
    const std::string& command = args[i];
    auto needs = [&](size_t count) {
      if (i + count >= args.size()) {
        std::println(stderr, "pointer-client: '{}' needs {} argument(s)", command, count);
        std::exit(EXIT_FAILURE);
      }
    };

    if (command == "move") {
      needs(2);
      const auto x = static_cast<uint32_t>(std::atoi(args[i + 1].c_str()));
      const auto y = static_cast<uint32_t>(std::atoi(args[i + 2].c_str()));
      i += 2;
      zwlr_virtual_pointer_v1_motion_absolute(pointer, nextTime(), x, y, extentWidth, extentHeight);
    } else if (command == "press" || command == "release" || command == "click") {
      needs(1);
      const auto button = static_cast<uint32_t>(std::atoi(args[i + 1].c_str()));
      i += 1;
      if (command != "release") {
        zwlr_virtual_pointer_v1_button(pointer, nextTime(), button, WL_POINTER_BUTTON_STATE_PRESSED);
      }
      if (command == "click") {
        zwlr_virtual_pointer_v1_frame(pointer);
        wl_display_roundtrip(display);
      }
      if (command != "press") {
        zwlr_virtual_pointer_v1_button(pointer, nextTime(), button, WL_POINTER_BUTTON_STATE_RELEASED);
      }
    } else {
      std::println(stderr, "pointer-client: unknown command '{}'", command);
      return EXIT_FAILURE;
    }

    zwlr_virtual_pointer_v1_frame(pointer);
    if (wl_display_roundtrip(display) < 0) {
      std::println(stderr, "pointer-client: connection lost");
      return EXIT_FAILURE;
    }
  }

  zwlr_virtual_pointer_v1_destroy(pointer);
  wl_display_roundtrip(display);
  wl_display_disconnect(display);
  return EXIT_SUCCESS;
}
