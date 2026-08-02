#include "core/log.h"
#include "server/server.h"
#include "wlr.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>

namespace {
  umbriel::Server* g_server = nullptr;
  constexpr Logger kLog("main");

  void onSignal(int /*signal*/) {
    if (g_server != nullptr) {
      g_server->stop();
    }
  }

  void printUsage(const char* argv0) { kLog.error("Usage: {} [-s startup_command]", argv0); }
} // namespace

int main(int argc, char** argv) {
  initLogFile();
#ifdef NDEBUG
  wlr_log_init(WLR_INFO, nullptr);
#else
  wlr_log_init(WLR_DEBUG, nullptr);
#endif

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
    kLog.info("starting umbriel");
    umbriel::Server server;
    g_server = &server;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    if (!server.start(startupCmd)) {
      kLog.error("failed to start server");
      return EXIT_FAILURE;
    }

    server.run();
    g_server = nullptr;
    kLog.info("shutting down");
    return EXIT_SUCCESS;
  } catch (const std::exception& ex) {
    kLog.error("{}", ex.what());
    return EXIT_FAILURE;
  }
}
