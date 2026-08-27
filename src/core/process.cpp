#include "core/process.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace umbriel {

  void resetChildSignalState() {
    struct sigaction action{};
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    sigaction(SIGCHLD, &action, nullptr);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
  }

  std::string resolveExecutable(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return {};
    }
    if (std::strchr(name, '/') != nullptr) {
      return access(name, X_OK) == 0 ? name : "";
    }
    const char* pathEnv = std::getenv("PATH");
    std::string path = pathEnv != nullptr ? pathEnv : "/bin:/usr/bin";
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find(':', start);
      const std::size_t count = (end == std::string::npos ? path.size() : end) - start;
      const std::string dir = count == 0 ? "." : path.substr(start, count);
      start = end == std::string::npos ? path.size() + 1 : end + 1;
      const std::string candidate = dir + "/" + name;
      if (access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    return {};
  }

} // namespace umbriel
