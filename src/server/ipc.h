#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>
#include <vector>

struct wl_event_source;

namespace umbriel {

  class Server;

  class Ipc {
  public:
    // One bit per subscribable event name; the subscribe request ORs the bits
    // it asked for into the connection. Keep the names below in sync with the
    // event builders in ipc.cpp.
    enum : uint8_t {
      kEventTheme = 1 << 0,
      kEventOverview = 1 << 1,
      kEventKeyboardLayout = 1 << 2,
      kEventWindows = 1 << 3,
    };

    Ipc(Server& server, const std::string& waylandSocketName);
    ~Ipc();

    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;

    void notifyThemeChanged();
    void notifyOverviewChanged();
    void notifyKeyboardLayoutChanged();
    void notifyWindowsChanged();

  private:
    struct Connection {
      Ipc* owner = nullptr;
      int fd = -1;
      std::string input;
      std::string output;
      size_t writeOffset = 0;
      wl_event_source* fdSource = nullptr;
      wl_event_source* deadline = nullptr;
      bool responding = false;
      uint8_t subscribedEvents = 0;
    };

    static int onListenReadable(int fd, uint32_t mask, void* data);
    static int onConnectionEvent(int fd, uint32_t mask, void* data);
    static int onConnectionTimeout(void* data);

    void acceptConnections();
    void addConnection(int clientFd);
    bool readRequest(Connection& connection);
    bool writeResponse(Connection& connection);
    void prepareResponse(Connection& connection, std::string response);
    void removeConnection(Connection* connection);
    static void closeConnection(Connection& connection);
    std::string handleRequest(Connection& connection, std::string_view line);
    void broadcastEvent(uint8_t event, const nlohmann::json& payload);

    Server* m_server;
    std::string m_socketPath;
    int m_listenFd = -1;
    wl_event_source* m_eventSource = nullptr;
    std::vector<std::unique_ptr<Connection>> m_connections;
  };

} // namespace umbriel
