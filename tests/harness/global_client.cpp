#include <cstdio>
#include <cstring>
#include <string_view>
#include <wayland-client.h>

namespace {
  struct State {
    std::string_view wanted;
    bool found = false;
  };

  void handleGlobal(void* data, wl_registry*, uint32_t, const char* interface, uint32_t) {
    auto* state = static_cast<State*>(data);
    if (state->wanted == interface) {
      state->found = true;
    }
  }

  void handleGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener{
      .global = handleGlobal,
      .global_remove = handleGlobalRemove,
  };
} // namespace

int main(int argc, char** argv) {
  if (argc != 3 || (std::strcmp(argv[2], "present") != 0 && std::strcmp(argv[2], "absent") != 0)) {
    std::fprintf(stderr, "usage: global-client INTERFACE present|absent\n");
    return 2;
  }

  wl_display* display = wl_display_connect(nullptr);
  if (display == nullptr) {
    std::fprintf(stderr, "global-client: cannot connect to WAYLAND_DISPLAY\n");
    return 2;
  }

  State state{.wanted = argv[1]};
  wl_registry* registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  const bool roundtripOk = wl_display_roundtrip(display) >= 0;
  wl_registry_destroy(registry);
  wl_display_disconnect(display);

  if (!roundtripOk) {
    std::fprintf(stderr, "global-client: registry roundtrip failed\n");
    return 2;
  }

  const bool expected = std::strcmp(argv[2], "present") == 0;
  if (state.found != expected) {
    std::fprintf(
        stderr, "global-client: %.*s was %s, expected %s\n", static_cast<int>(state.wanted.size()), state.wanted.data(),
        state.found ? "present" : "absent", expected ? "present" : "absent"
    );
    return 1;
  }
  return 0;
}
