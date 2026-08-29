#pragma once

#include <optional>
#include <xkbcommon/xkbcommon.h>

namespace umbriel {

  // Find the target group with the same XKB layout name as sourceGroup.
  // Nullopt keeps deliberately different per-device layout vocabularies apart.
  [[nodiscard]] std::optional<xkb_layout_index_t>
  matchingLayoutGroup(xkb_keymap* target, xkb_keymap* source, xkb_layout_index_t sourceGroup);

} // namespace umbriel
