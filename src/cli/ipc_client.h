#pragma once
#include <string_view>

namespace umbriel {

  enum class IpcCommand { Apps, Layers, Action };

  int runIpcCommand(IpcCommand cmd, std::string_view arg = {}, bool json = false);

} // namespace umbriel
