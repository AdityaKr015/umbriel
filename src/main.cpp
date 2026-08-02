#include "server/server.hpp"

#include "wlr.hpp"

#include <csignal>
#include <cstdlib>
#include <exception>

namespace {
umbriel::Server* g_server = nullptr;

void onSignal(int /*signal*/) {
  if (g_server != nullptr) {
    g_server->stop();
  }
}
} // namespace

int main(int /*argc*/, char** /*argv*/) {
  wlr_log_init(WLR_DEBUG, nullptr);

  try {
    umbriel::Server server;
    g_server = &server;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!server.start()) {
      return EXIT_FAILURE;
    }

    server.run();
    g_server = nullptr;
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    wlr_log(WLR_ERROR, "%s", ex.what());
    return EXIT_FAILURE;
  }
}
