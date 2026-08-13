#include "config/store.h"

#include <algorithm>
#include <utility>

namespace umbriel {

  void ConfigStore::beginLoad() {
    m_diagnostics.clear();
    m_watchPaths.clear();
    m_watchPaths.push_back(m_rootPath);
  }

  void ConfigStore::addDiagnostic(ConfigDiagnostic diagnostic) { m_diagnostics.push_back(std::move(diagnostic)); }

  void ConfigStore::addWatchPath(std::filesystem::path path) {
    if (std::ranges::find(m_watchPaths, path) == m_watchPaths.end()) {
      m_watchPaths.push_back(std::move(path));
    }
  }

  void ConfigStore::commit(Config&& config, bool fileMissing) {
    m_config = std::move(config);
    m_fileMissing = fileMissing;
    ++m_generation;
  }

  void ConfigStore::setRootPath(std::filesystem::path path, bool explicitPath) {
    m_rootPath = std::move(path);
    m_explicitPath = explicitPath;
  }

} // namespace umbriel
