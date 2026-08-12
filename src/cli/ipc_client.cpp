#include "cli/ipc_client.h"

#include "server/ipc_commands.h"

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <print>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace umbriel {

  namespace {
    std::string resolveSocketPath() {
      if (const char* sock = std::getenv("UMBRIEL_SOCKET"); sock != nullptr && sock[0] != '\0') {
        return sock;
      }
      const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
      const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
      if (runtimeDir != nullptr && runtimeDir[0] != '\0' && waylandDisplay != nullptr && waylandDisplay[0] != '\0') {
        return std::string(runtimeDir) + "/umbriel-" + waylandDisplay + ".sock";
      }
      return {};
    }
  } // namespace

  int runIpcCommand(const IpcCommandSpec& spec, std::string_view arg, bool json) {
    std::string socketPath = resolveSocketPath();
    if (socketPath.empty()) {
      std::println(stderr, "error: cannot find umbriel socket (is the compositor running?)");
      return EXIT_FAILURE;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      std::println(stderr, "error: failed to create socket");
      return EXIT_FAILURE;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path)) {
      std::println(stderr, "error: socket path too long");
      close(fd);
      return EXIT_FAILURE;
    }
    socketPath.copy(addr.sun_path, socketPath.size());

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      std::println(stderr, "error: cannot connect to umbriel socket (is the compositor running?)");
      close(fd);
      return EXIT_FAILURE;
    }

    // Set receive timeout.
    timeval tv{};
    tv.tv_sec = 2;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // Build request.
    nlohmann::json req;
    req["cmd"] = spec.name;
    if (spec.takesArg) {
      req["arg"] = std::string(arg);
    }
    std::string payload = req.dump() + "\n";

    // Send.
    size_t sent = 0;
    while (sent < payload.size()) {
      ssize_t n = send(fd, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        std::println(stderr, "error: failed to send request");
        close(fd);
        return EXIT_FAILURE;
      }
      sent += static_cast<size_t>(n);
    }

    // Receive.
    std::string buf;
    char chunk[4096];
    while (true) {
      ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        break;
      }
      buf.append(chunk, static_cast<size_t>(n));
    }
    close(fd);

    if (buf.empty()) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    // Strip trailing newline.
    if (buf.back() == '\n') {
      buf.pop_back();
    }

    auto resp = nlohmann::json::parse(buf, nullptr, false);
    if (resp.is_discarded()) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    if (resp.contains("err")) {
      std::println(stderr, "error: {}", resp["err"].get<std::string>());
      return EXIT_FAILURE;
    }

    if (!resp.contains("ok")) {
      std::println(stderr, "error: malformed response");
      return EXIT_FAILURE;
    }

    const auto& ok = resp["ok"];

    if (json) {
      std::println("{}", ok.dump());
      return EXIT_SUCCESS;
    }

    if (spec.printHuman != nullptr) {
      spec.printHuman(ok);
    }

    return EXIT_SUCCESS;
  }

} // namespace umbriel
