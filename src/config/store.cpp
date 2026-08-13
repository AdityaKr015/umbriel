#include "config/store.h"

#include <algorithm>
#include <string_view>
#include <tuple>
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

  ConfigChange ConfigChange::between(const Config& before, const Config& after) {
    return {
        .appearance = before.appearance != after.appearance,
        .overview = before.overview != after.overview,
        .layout = before.layout != after.layout,
        .workspaces = before.workspaces != after.workspaces,
        .general = before.general != after.general,
        .environment = before.environment != after.environment,
        .input = before.input != after.input,
        .keybinds = before.keybinds != after.keybinds,
        .outputs = before.outputs != after.outputs,
        .windowRules = before.windowRules != after.windowRules,
        .layerRules = before.layerRules != after.layerRules,
        .workspaceRules = before.workspaceRules != after.workspaceRules,
    };
  }

  std::string ConfigChange::summary() const {
    std::string out;
    const auto add = [&out](bool changed, std::string_view name) {
      if (!changed) {
        return;
      }
      if (!out.empty()) {
        out += ", ";
      }
      out += name;
    };
    add(appearance, "appearance");
    add(overview, "overview");
    add(layout, "layout");
    add(workspaces, "workspaces");
    add(general, "general");
    add(environment, "environment");
    add(input, "input");
    add(keybinds, "keybinds");
    add(outputs, "outputs");
    add(windowRules, "window rules");
    add(layerRules, "layer rules");
    add(workspaceRules, "workspace rules");
    return out;
  }

  void ConfigStore::sortDiagnostics() {
    // Stable so two diagnostics on the same key keep the order they were found
    // in. File-less entries (whole-config errors) sort first.
    std::ranges::stable_sort(m_diagnostics, [](const ConfigDiagnostic& a, const ConfigDiagnostic& b) {
      return std::tie(a.file, a.line, a.column) < std::tie(b.file, b.line, b.column);
    });
  }

  void ConfigStore::commit(Config&& config, bool fileMissing) {
    // Computed before the move, and only after the first load: everything is new
    // the first time through.
    m_lastChange = m_generation == 0 ? ConfigChange::everything() : ConfigChange::between(m_config, config);
    m_config = std::move(config);
    m_fileMissing = fileMissing;
    ++m_generation;
  }

  void ConfigStore::setRootPath(std::filesystem::path path, bool explicitPath) {
    m_rootPath = std::move(path);
    m_explicitPath = explicitPath;
  }

} // namespace umbriel
