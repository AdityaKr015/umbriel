#include "core/process.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace umbriel {

  void resetChildSignalState() {
    std::signal(SIGCHLD, SIG_DFL);
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, nullptr);
  }

  bool executableOnPath(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }
    // Absolute / relative path: check directly.
    if (std::strchr(name, '/') != nullptr) {
      return access(name, X_OK) == 0;
    }
    const char* pathEnv = std::getenv("PATH");
    if (pathEnv == nullptr || pathEnv[0] == '\0') {
      return false;
    }
    std::string path = pathEnv;
    std::size_t start = 0;
    while (start <= path.size()) {
      const std::size_t end = path.find(':', start);
      const std::size_t count = (end == std::string::npos ? path.size() : end) - start;
      const std::string dir = path.substr(start, count);
      start = end == std::string::npos ? path.size() + 1 : end + 1;
      if (dir.empty()) {
        continue;
      }
      const std::filesystem::path candidate = std::filesystem::path(dir) / name;
      std::error_code ec;
      if (std::filesystem::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) {
        return true;
      }
    }
    return false;
  }

} // namespace umbriel
