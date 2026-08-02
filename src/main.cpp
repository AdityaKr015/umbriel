#include "server/server.h"

#include "wlr.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace {
umbriel::Server* g_server = nullptr;

void onSignal(int /*signal*/) {
  if (g_server != nullptr) {
    g_server->stop();
  }
}

void printUsage(const char* argv0) {
  wlr_log(WLR_ERROR, "Usage: %s [-s startup_command]", argv0);
}
} // namespace

int main(int argc, char** argv) {
  wlr_log_init(WLR_INFO, nullptr);

  const char* startupCmd = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      startupCmd = argv[++i];
    } else {
      printUsage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  try {
    umbriel::Server server;
    g_server = &server;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!server.start(startupCmd)) {
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
