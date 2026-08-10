#include "cli/ipc_client.h"
#include "cli/outputs.h"
#include "config/config.h"
#include "config/config_diag.h"
#include "core/fdlimit.h"
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
#include <vector>

#ifndef UMBRIEL_VERSION
#define UMBRIEL_VERSION "unknown"
#endif

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
        std::println(stderr, "error: unknown option '{}' for validate", argv[i]);
        return EXIT_FAILURE;
      }
    }

    umbriel::loadConfig(configPath);
    const auto& diags = umbriel::configDiagnostics();
    if (diags.empty()) {
      std::println("config: ok ({})", umbriel::configRootPath().string());
      return EXIT_SUCCESS;
    }
    bool hasError = false;
    for (const auto& d : diags) {
      const std::string loc = d.location();
      if (d.severity == umbriel::ConfigDiagnostic::Severity::Error) {
        hasError = true;
        std::println(stderr, "error: {}{}", loc.empty() ? "" : loc + ": ", d.message);
      } else {
        std::println(stderr, "warning: {}{}", loc.empty() ? "" : loc + ": ", d.message);
      }
    }
    return hasError ? EXIT_FAILURE : EXIT_SUCCESS;
  }

  void printHelp(FILE* stream) {
    std::println(
        stream,
        "umbriel {} — a wayland compositor\n"
        "\n"
        "Usage: umbriel [-s <command>] [-c <config>]   run the compositor\n"
        "       umbriel validate [-c <config>]          check the config file\n"
        "       umbriel outputs                          list outputs and modes\n"
        "       umbriel apps                             list running application ids\n"
        "       umbriel layers                           list layer-shell surfaces\n"
        "       umbriel msg <action> [args...]           send an action to the compositor\n"
        "       umbriel actions                           list available actions\n"
        "       umbriel help | -h | --help               show this help\n"
        "       umbriel --version                        print version\n"
        "\n"
        "Options:\n"
        "  -s <command>   spawn <command> once the compositor starts\n"
        "  -c <config>    use <config> instead of the default config path\n"
        "\n"
        "Run `umbriel actions` to list all available actions for `msg` and keybinds.",
        UMBRIEL_VERSION
    );
  }

  int printActions() {
    for (const auto& spec : umbriel::actionSpecs()) {
      if (spec.param.empty()) {
        std::println("{}", spec.name);
      } else {
        std::println("{}:{}", spec.name, spec.param);
      }
    }
    return EXIT_SUCCESS;
  }
} // namespace

int main(int argc, char** argv) {
  if (argc >= 2) {
    if (std::strcmp(argv[1], "validate") == 0) {
      return validateConfig(argc, argv);
    }
    if (std::strcmp(argv[1], "outputs") == 0) {
      return umbriel::runOutputsCommand();
    }
    if (std::strcmp(argv[1], "actions") == 0) {
      return printActions();
    }
    if (std::strcmp(argv[1], "help") == 0 || std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0) {
      printHelp(stdout);
      return EXIT_SUCCESS;
    }
    if (std::strcmp(argv[1], "--version") == 0 || std::strcmp(argv[1], "-V") == 0) {
      std::println("umbriel {}", UMBRIEL_VERSION);
      return EXIT_SUCCESS;
    }

    // IPC subcommands: apps, layers, msg
    auto isJsonFlag = [](const char* arg) { return std::strcmp(arg, "--json") == 0 || std::strcmp(arg, "-j") == 0; };
    auto isHelpFlag = [](const char* arg) { return std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0; };

    if (std::strcmp(argv[1], "apps") == 0 || std::strcmp(argv[1], "layers") == 0) {
      bool jf = false;
      for (int i = 2; i < argc; ++i) {
        if (isHelpFlag(argv[i])) {
          printHelp(stdout);
          return EXIT_SUCCESS;
        }
        if (isJsonFlag(argv[i])) {
          jf = true;
        } else {
          printHelp(stderr);
          return EXIT_FAILURE;
        }
      }
      auto cmd = std::strcmp(argv[1], "apps") == 0 ? umbriel::IpcCommand::Apps : umbriel::IpcCommand::Layers;
      return umbriel::runIpcCommand(cmd, {}, jf);
    }
    if (std::strcmp(argv[1], "msg") == 0) {
      // Help only when -h/--help is the immediate argument (not buried in spawn args).
      if (argc == 3 && isHelpFlag(argv[2])) {
        std::println("Usage: umbriel msg <action> [args...]");
        std::println("");
        std::println("Send an action to the running compositor.");
        std::println("Use `msg spawn <cmd...>` as shorthand for `msg spawn:<cmd...>`.");
        std::println("");
        std::println("Available actions:");
        for (const auto& spec : umbriel::actionSpecs()) {
          if (spec.param.empty()) {
            std::println("  {}", spec.name);
          } else {
            std::println("  {}:{}", spec.name, spec.param);
          }
        }
        return EXIT_SUCCESS;
      }

      bool jf = false;
      // Collect non-flag args; for spawn, everything after "spawn" is literal.
      std::vector<const char*> args;
      bool inSpawnTail = false;
      for (int i = 2; i < argc; ++i) {
        if (!inSpawnTail && isJsonFlag(argv[i])) {
          jf = true;
        } else {
          args.push_back(argv[i]);
          // Once we see "spawn" as the first non-flag arg and there are more,
          // everything after belongs to the command (no flag stripping).
          if (args.size() == 1 && std::strcmp(argv[i], "spawn") == 0) {
            inSpawnTail = true;
          }
        }
      }

      if (args.empty()) {
        printHelp(stderr);
        return EXIT_FAILURE;
      }

      std::string actionString;
      if (std::strcmp(args[0], "spawn") == 0 && args.size() > 1) {
        actionString = "spawn:";
        for (size_t i = 1; i < args.size(); ++i) {
          if (i > 1) {
            actionString += ' ';
          }
          actionString += args[i];
        }
      } else {
        for (size_t i = 0; i < args.size(); ++i) {
          if (i > 0) {
            actionString += ' ';
          }
          actionString += args[i];
        }
      }
      return umbriel::runIpcCommand(umbriel::IpcCommand::Action, actionString, jf);
    }
  }

  // If argv[1] exists and doesn't start with '-', it's an unknown subcommand.
  if (argc >= 2 && argv[1][0] != '-') {
    printHelp(stderr);
    return EXIT_FAILURE;
  }

  initLogFile();
#ifdef NDEBUG
  wlr_log_init(WLR_INFO, wlrLogHandler);
#else
  wlr_log_init(WLR_DEBUG, wlrLogHandler);
#endif

  raiseFileDescriptorLimit();

  const char* startupCmd = nullptr;
  const char* configPath = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
      startupCmd = argv[++i];
    } else if (std::strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
      configPath = argv[++i];
    } else {
      printHelp(stderr);
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
