#include "cli/outputs.h"
#include "config/config.h"
#include "config/config_diag.h"
#include "core/log.h"
#include "server/server.h"
#include "wlr.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <print>
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
        std::println(stderr, "Usage: {} validate [-c config_file]", argv[0]);
        return EXIT_FAILURE;
      }
    }

    setConsoleLogging(false);
    umbriel::loadConfig(configPath);

    const auto& diagnostics = umbriel::configDiagnostics();
    const auto& rootPath = umbriel::configRootPath();

    if (configPath == nullptr && diagnostics.empty() && !std::filesystem::is_regular_file(rootPath)) {
      std::println("no config file found: {} (defaults will be used)", rootPath.c_str());
      return EXIT_SUCCESS;
    }

    int errors = 0;
    int warnings = 0;
    for (const auto& diag : diagnostics) {
      const char* severity = diag.severity == umbriel::ConfigDiagnostic::Severity::Error ? "error" : "warning";
      const std::string loc = diag.location();
      if (loc.empty()) {
        std::println("{}: {}", severity, diag.message);
      } else {
        std::println("{}: {}: {}", loc, severity, diag.message);
      }
      if (diag.severity == umbriel::ConfigDiagnostic::Severity::Error) {
        ++errors;
      } else {
        ++warnings;
      }
    }

    if (diagnostics.empty()) {
      std::println("config OK: {}", rootPath.c_str());
    } else {
      std::println("{} error(s), {} warning(s)", errors, warnings);
    }
    return errors > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  void printUsage(const char* argv0) {
    kLog.error(
        "Usage: {} [-s startup_command] [-c config_file]\n       {} validate [-c config_file]\n       {} outputs",
        argv0, argv0, argv0
    );
  }
} // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "validate") == 0) {
    return validateConfig(argc, argv);
  }
  if (argc >= 2 && std::strcmp(argv[1], "outputs") == 0) {
    return umbriel::runOutputsCommand();
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
