#pragma once
#include <cstdint>
#include <string>

struct wl_event_source;

namespace umbriel {

  class Server;

  class Ipc {
  public:
    Ipc(Server& server, const std::string& waylandSocketName);
    ~Ipc();

    Ipc(const Ipc&) = delete;
    Ipc& operator=(const Ipc&) = delete;

  private:
    static int onListenReadable(int fd, uint32_t mask, void* data);
    void handleClient(int clientFd);
    std::string handleRequest(std::string_view line);

    Server* m_server;
    std::string m_socketPath;
    int m_listenFd = -1;
    wl_event_source* m_eventSource = nullptr;
  };

} // namespace umbriel
