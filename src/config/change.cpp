#include "config/change.h"

#include <string_view>

namespace umbriel {

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

} // namespace umbriel
