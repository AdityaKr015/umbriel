#pragma once
#include <string_view>

namespace umbriel {

  struct IpcCommandSpec;

  int runIpcCommand(const IpcCommandSpec& spec, std::string_view arg = {}, bool json = false);

} // namespace umbriel
