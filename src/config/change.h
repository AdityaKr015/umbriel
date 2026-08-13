#pragma once

#include "config/config.h"

#include <string>

namespace umbriel {

  // Which source sections a reload altered. Runtime effects are derived
  // separately because one section can invalidate several subsystems.
  struct ConfigChange {
    bool appearance = true;
    bool overview = true;
    bool layout = true;
    bool workspaces = true;
    bool general = true;
    bool environment = true;
    bool input = true;
    bool keybinds = true;
    bool outputs = true;
    bool windowRules = true;
    bool layerRules = true;
    bool workspaceRules = true;

    [[nodiscard]] bool any() const {
      return appearance
          || overview
          || layout
          || workspaces
          || general
          || environment
          || input
          || keybinds
          || outputs
          || windowRules
          || layerRules
          || workspaceRules;
    }

    // Comma-separated names of the sections that changed, empty when none did.
    [[nodiscard]] std::string summary() const;

    // What a first load reports: everything is new.
    [[nodiscard]] static ConfigChange everything() { return {}; }
    [[nodiscard]] static ConfigChange between(const Config& before, const Config& after);
  };

} // namespace umbriel
