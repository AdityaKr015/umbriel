#pragma once

#include "config/config.h"
#include "config/effects.h"

#include <functional>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

namespace umbriel {

  // A selector recorded while parsing and checked once every section is
  // read, so forward and cross-include references resolve.
  struct EffectReference {
    std::string context;
    std::string name;
    EffectKind kind;
    bool allowOff;
    toml::source_region source;
    std::function<void()> clear;
  };

  namespace registry {

    // What one load shares across tables.
    struct ReadContext {
      // The config being loaded. Tables read earlier are already in it, such as the scratchpads an action may name.
      Config& loaded;
      std::vector<EffectReference>& effectReferences;
      // Set by a field to drop the rule entry being read.
      bool entryRejected = false;
    };

  } // namespace registry

} // namespace umbriel
