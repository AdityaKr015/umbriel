// Maps an xdg toplevel, then on a compositor close request unmaps it without
// destroying the surface. The compositor therefore keeps the View registered
// and unmapped, which is what makes this client useful: closing a window this
// way never reaches Server::removeView, so any focus reassignment the
// compositor performs must happen at unmap time, the same point a card
// disappears from the overview.
//
// Prints "mapped" once the toplevel is up and "unmapped" once the close
// request lands, then keeps the connection alive until the harness kills it.

#include "xdg-shell-client-protocol.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <print>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

namespace {

  constexpr int kSurfaceSize = 64;

  struct Buffer {
    wl_buffer* resource = nullptr;
    void* pixels = MAP_FAILED;
    size_t size = 0;
  };

  struct State {
    wl_display* display = nullptr;
    wl_compositor* compositor = nullptr;
    wl_shm* shm = nullptr;
    xdg_wm_base* wmBase = nullptr;
    wl_surface* surface = nullptr;
    xdg_surface* xdgSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    Buffer buffer;
    bool mapped = false;
    bool closed = false;
  };

  Buffer createBuffer(State& state) {
    Buffer buffer;
    const int stride = kSurfaceSize * 4;
    buffer.size = static_cast<size_t>(stride * kSurfaceSize);
    const int fd = memfd_create("umbriel-unmap-client", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(buffer.size)) < 0) {
      if (fd >= 0) {
        close(fd);
      }
      return buffer;
    }

    buffer.pixels = mmap(nullptr, buffer.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buffer.pixels == MAP_FAILED) {
      close(fd);
      return buffer;
    }
    std::fill_n(static_cast<uint32_t*>(buffer.pixels), buffer.size / sizeof(uint32_t), 0xFF5577AA);

    wl_shm_pool* pool = wl_shm_create_pool(state.shm, fd, static_cast<int>(buffer.size));
    buffer.resource = wl_shm_pool_create_buffer(pool, 0, kSurfaceSize, kSurfaceSize, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
  }

  void xdgSurfaceConfigure(void* data, xdg_surface* xdgSurface, uint32_t serial) {
    auto& state = *static_cast<State*>(data);
    xdg_surface_ack_configure(xdgSurface, serial);
    if (state.mapped || state.closed) {
      return;
    }
    state.mapped = true;
    wl_surface_attach(state.surface, state.buffer.resource, 0, 0);
    wl_surface_damage_buffer(state.surface, 0, 0, kSurfaceSize, kSurfaceSize);
    wl_surface_commit(state.surface);
    std::println("mapped");
    std::fflush(stdout);
  }

  constexpr xdg_surface_listener kXdgSurfaceListener = {
      .configure = xdgSurfaceConfigure,
  };

  void toplevelConfigure(void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}

  void toplevelClose(void* data, xdg_toplevel*) {
    auto& state = *static_cast<State*>(data);
    if (!state.mapped) {
      return;
    }
    state.mapped = false;
    state.closed = true;
    // Attaching a null buffer unmaps the surface; the toplevel stays alive.
    wl_surface_attach(state.surface, nullptr, 0, 0);
    wl_surface_commit(state.surface);
    std::println("unmapped");
    std::fflush(stdout);
  }

  constexpr xdg_toplevel_listener kToplevelListener = {
      .configure = toplevelConfigure,
      .close = toplevelClose,
      .configure_bounds = nullptr,
      .wm_capabilities = nullptr,
  };

  void wmBasePing(void*, xdg_wm_base* wmBase, uint32_t serial) { xdg_wm_base_pong(wmBase, serial); }

  constexpr xdg_wm_base_listener kWmBaseListener = {
      .ping = wmBasePing,
  };

  void registryGlobal(void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    auto& state = *static_cast<State*>(data);
    if (std::strcmp(interface, wl_compositor_interface.name) == 0) {
      state.compositor = static_cast<wl_compositor*>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(interface, wl_shm_interface.name) == 0) {
      state.shm = static_cast<wl_shm*>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0) {
      state.wmBase =
          static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, std::min(version, 1U)));
      xdg_wm_base_add_listener(state.wmBase, &kWmBaseListener, &state);
    }
  }

  void registryGlobalRemove(void*, wl_registry*, uint32_t) {}

  constexpr wl_registry_listener kRegistryListener = {
      .global = registryGlobal,
      .global_remove = registryGlobalRemove,
  };

  void destroyBuffer(Buffer& buffer) {
    if (buffer.resource != nullptr) {
      wl_buffer_destroy(buffer.resource);
    }
    if (buffer.pixels != MAP_FAILED) {
      munmap(buffer.pixels, buffer.size);
    }
  }

} // namespace

int main() {
  State state;
  state.display = wl_display_connect(nullptr);
  if (state.display == nullptr) {
    std::println(stderr, "unmap-client: cannot connect to WAYLAND_DISPLAY");
    return EXIT_FAILURE;
  }

  wl_registry* registry = wl_display_get_registry(state.display);
  wl_registry_add_listener(registry, &kRegistryListener, &state);
  wl_display_roundtrip(state.display);
  wl_display_roundtrip(state.display);

  if (state.compositor == nullptr || state.shm == nullptr || state.wmBase == nullptr) {
    std::println(stderr, "unmap-client: compositor is missing a required Wayland global");
    return EXIT_FAILURE;
  }

  state.buffer = createBuffer(state);
  if (state.buffer.resource == nullptr) {
    std::println(stderr, "unmap-client: failed to allocate shared-memory buffer");
    return EXIT_FAILURE;
  }

  state.surface = wl_compositor_create_surface(state.compositor);
  state.xdgSurface = xdg_wm_base_get_xdg_surface(state.wmBase, state.surface);
  xdg_surface_add_listener(state.xdgSurface, &kXdgSurfaceListener, &state);
  state.toplevel = xdg_surface_get_toplevel(state.xdgSurface);
  xdg_toplevel_add_listener(state.toplevel, &kToplevelListener, &state);
  xdg_toplevel_set_title(state.toplevel, "unmap-client");
  wl_surface_commit(state.surface);

  while (wl_display_dispatch(state.display) >= 0) {
  }

  if (state.toplevel != nullptr) {
    xdg_toplevel_destroy(state.toplevel);
  }
  if (state.xdgSurface != nullptr) {
    xdg_surface_destroy(state.xdgSurface);
  }
  if (state.surface != nullptr) {
    wl_surface_destroy(state.surface);
  }
  destroyBuffer(state.buffer);
  wl_display_disconnect(state.display);
  return EXIT_SUCCESS;
}
