#include "config/config.h"
#include "config/config_diag.h"
#include "core/log.h"
#include "server/server.h"
#include "wlr.h"

#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <exception>
#include <string>

namespace {
  umbriel::Server* g_server = nullptr;
  constexpr Logger kLog("main");

  void onSignal(int /*signal*/) {
    if (g_server != nullptr) {
      g_server->stop();
    }
  }

  int validateConfig(int argc, char** argv) {
    const char* configPath = nullptr;
    for (int i = 2; i < argc; ++i) {
      if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
        configPath = argv[++i];
      } else {
        std::fprintf(stderr, "Usage: %s validate [-c config_file]\n", argv[0]);
        return EXIT_FAILURE;
      }
    }

    setConsoleLogging(false);
    umbriel::loadConfig(configPath);

    const auto& diagnostics = umbriel::configDiagnostics();
    const auto& rootPath = umbriel::configRootPath();

    if (configPath == nullptr && diagnostics.empty()
        && !std::filesystem::is_regular_file(rootPath)) {
      std::printf("no config file found: %s (defaults will be used)\n", rootPath.c_str());
      return EXIT_SUCCESS;
    }

    int errors = 0;
    int warnings = 0;
    for (const auto& diag : diagnostics) {
      const char* severity =
          diag.severity == umbriel::ConfigDiagnostic::Severity::Error ? "error" : "warning";
      const std::string loc = diag.location();
      if (loc.empty()) {
        std::printf("%s: %s\n", severity, diag.message.c_str());
      } else {
        std::printf("%s: %s: %s\n", loc.c_str(), severity, diag.message.c_str());
      }
      if (diag.severity == umbriel::ConfigDiagnostic::Severity::Error) {
        ++errors;
      } else {
        ++warnings;
      }
    }

    if (diagnostics.empty()) {
      std::printf("config OK: %s\n", rootPath.c_str());
    } else {
      std::printf("%d error(s), %d warning(s)\n", errors, warnings);
    }
    return errors > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  void printUsage(const char* argv0) {
    kLog.error(
        "Usage: {} [-s startup_command] [-c config_file]\n       {} validate [-c config_file]", argv0, argv0
    );
  }
} // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "validate") == 0) {
    return validateConfig(argc, argv);
  }

  initLogFile();
#ifdef NDEBUG
  wlr_log_init(WLR_INFO, nullptr);
#else
  wlr_log_init(WLR_DEBUG, nullptr);
#endif

  const char* startupCmd = nullptr;
  const char* configPath = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      startupCmd = argv[++i];
    } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      configPath = argv[++i];
    } else {
      printUsage(argv[0]);
      return EXIT_FAILURE;
    }
  }

  try {
    kLog.info("starting umbriel");
    umbriel::loadConfig(configPath);
    umbriel::Server server;
    g_server = &server;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGCHLD, SIG_IGN);

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
