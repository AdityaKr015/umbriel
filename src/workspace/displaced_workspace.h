#pragma once

#include "layout/layout.h"
#include "output/identity.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace umbriel {

  // Own the monitor identity while its Output is absent. Temporary refuge workspaces use their current numeric
  // position for selection, while layout rules and returning windows retain this original workspace identity.
  struct DisplacedWorkspace {
    std::string outputName;
    std::string outputMake;
    std::string outputModel;
    std::string outputSerial;
    std::string workspaceName;
    size_t workspaceIndex = 0;
    bool workspaceNamed = false;
    uint64_t configGeneration = 0;
    std::optional<LayoutMode> layoutModeOverride;

    [[nodiscard]] OutputIdentity identity() const {
      return {
          .connector = outputName,
          .make = outputMake,
          .model = outputModel,
          .serial = outputSerial,
      };
    }
  };

} // namespace umbriel
