#include "server/ipc.h"

#include "core/log.h"
#include "server/ipc_commands.h"
#include "server/server.h"

#include <cerrno>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wayland-server-core.h>

namespace umbriel {

  namespace {
    constexpr Logger kLog("ipc");
    constexpr size_t kMaxRequestSize = 65536;

  } // namespace

  Ipc::Ipc(Server& server, const std::string& waylandSocketName) : m_server(&server) {
    const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR");
    if (runtimeDir == nullptr || runtimeDir[0] == '\0') {
      kLog.error("XDG_RUNTIME_DIR not set, IPC socket disabled");
      return;
    }
    m_socketPath = std::string(runtimeDir) + "/umbriel-" + waylandSocketName + ".sock";

    m_listenFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (m_listenFd < 0) {
      kLog.error("failed to create IPC socket: {}", strerror(errno));
      return;
    }

    unlink(m_socketPath.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      kLog.error("IPC socket path too long");
      close(m_listenFd);
      m_listenFd = -1;
      return;
    }
    m_socketPath.copy(addr.sun_path, m_socketPath.size());

    if (bind(m_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      kLog.error("failed to bind IPC socket: {}", strerror(errno));
      close(m_listenFd);
      m_listenFd = -1;
      return;
    }

    if (listen(m_listenFd, 4) < 0) {
      kLog.error("failed to listen on IPC socket: {}", strerror(errno));
      close(m_listenFd);
      m_listenFd = -1;
      unlink(m_socketPath.c_str());
      return;
    }

    m_eventSource = wl_event_loop_add_fd(
        wl_display_get_event_loop(server.display()), m_listenFd, WL_EVENT_READABLE, onListenReadable, this
    );
    if (m_eventSource == nullptr) {
      kLog.error("failed to register IPC event source");
      close(m_listenFd);
      m_listenFd = -1;
      unlink(m_socketPath.c_str());
      return;
    }

    kLog.info("IPC listening on {}", m_socketPath);
  }

  Ipc::~Ipc() {
    if (m_eventSource != nullptr) {
      wl_event_source_remove(m_eventSource);
    }
    if (m_listenFd >= 0) {
      close(m_listenFd);
    }
    if (!m_socketPath.empty()) {
      unlink(m_socketPath.c_str());
    }
  }

  int Ipc::onListenReadable(int /*fd*/, uint32_t /*mask*/, void* data) {
    auto* ipc = static_cast<Ipc*>(data);
    while (true) {
      int clientFd = accept4(ipc->m_listenFd, nullptr, nullptr, SOCK_CLOEXEC);
      if (clientFd < 0) {
        break;
      }
      ipc->handleClient(clientFd);
    }
    return 0;
  }

  void Ipc::handleClient(int clientFd) {
    // Set timeouts so a stalled client cannot hang the compositor.
    timeval tv{};
    tv.tv_usec = 500000; // 500 ms
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    std::string buf;
    char chunk[4096];
    while (true) {
      ssize_t n = recv(clientFd, chunk, sizeof(chunk), 0);
      if (n <= 0) {
        break;
      }
      buf.append(chunk, static_cast<size_t>(n));
      if (buf.size() > kMaxRequestSize) {
        std::string resp = R"({"err":"request too long"})"
                           "\n";
        send(clientFd, resp.data(), resp.size(), MSG_NOSIGNAL);
        close(clientFd);
        return;
      }
      if (buf.contains('\n')) {
        break;
      }
    }

    // Extract up to the first newline.
    auto nlPos = buf.find('\n');
    std::string_view line = nlPos != std::string::npos ? std::string_view(buf).substr(0, nlPos) : std::string_view(buf);

    std::string resp = handleRequest(line) + "\n";

    size_t sent = 0;
    while (sent < resp.size()) {
      ssize_t n = send(clientFd, resp.data() + sent, resp.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) {
        break;
      }
      sent += static_cast<size_t>(n);
    }

    close(clientFd);
  }

  std::string Ipc::handleRequest(std::string_view line) {
    auto req = nlohmann::json::parse(line, nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("cmd") || !req["cmd"].is_string()) {
      return R"({"err":"malformed request"})";
    }
    const std::string cmd = req["cmd"].get<std::string>();
    const IpcCommandSpec* spec = findIpcCommand(cmd);
    if (spec == nullptr) {
      return nlohmann::json{{"err", "unknown command: " + cmd}}.dump();
    }
    std::string arg;
    if (spec->takesArg) {
      if (!req.contains("arg") || !req["arg"].is_string()) {
        return R"({"err":"malformed request"})";
      }
      arg = req["arg"].get<std::string>();
    }
    return spec->handle(*m_server, arg).dump();
  }

} // namespace umbriel
