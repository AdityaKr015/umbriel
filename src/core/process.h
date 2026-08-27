#pragma once

#include <string>

namespace umbriel {

  // Undo everything the compositor did to the process that a child must not inherit. `wl_event_loop_add_signal` blocks
  // SIGINT/SIGTERM process-wide via sigprocmask, and a blocked mask survives fork and exec, so without this every
  // spawned application would silently ignore Ctrl+C and systemd's stop signal. Call this between fork and exec,
  // alongside `restoreFileDescriptorLimit`.
  void resetChildSignalState();

  // Resolve a bare executable against PATH, or validate an explicit path. An
  // empty result means no executable was found.
  [[nodiscard]] std::string resolveExecutable(const char* name);

} // namespace umbriel
