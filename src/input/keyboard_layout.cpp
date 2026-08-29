#include "input/keyboard_layout.h"

#include <cstring>

namespace umbriel {

  std::optional<xkb_layout_index_t>
  matchingLayoutGroup(xkb_keymap* target, xkb_keymap* source, xkb_layout_index_t sourceGroup) {
    if (target == nullptr || source == nullptr || sourceGroup >= xkb_keymap_num_layouts(source)) {
      return std::nullopt;
    }
    const char* sourceName = xkb_keymap_layout_get_name(source, sourceGroup);
    if (sourceName == nullptr) {
      return std::nullopt;
    }
    const xkb_layout_index_t count = xkb_keymap_num_layouts(target);
    for (xkb_layout_index_t group = 0; group < count; ++group) {
      const char* name = xkb_keymap_layout_get_name(target, group);
      if (name != nullptr && std::strcmp(name, sourceName) == 0) {
        return group;
      }
    }
    return std::nullopt;
  }

} // namespace umbriel
