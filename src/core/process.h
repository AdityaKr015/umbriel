#pragma once

namespace umbriel {

  // Undo everything the compositor did to the process that a child must not
  // inherit. `wl_event_loop_add_signal` blocks SIGINT/SIGTERM process-wide via
  // sigprocmask, and a blocked mask survives fork and exec, so without this every
  // spawned application would silently ignore Ctrl+C and systemd's stop signal.
  //
  // Call this between fork and exec, alongside `restoreFileDescriptorLimit`.
  void resetChildSignalState();

  // Would `execlp(name, ...)` find something to run? Handles both a bare name
  // resolved against PATH and an explicit path.
  //
  // Inherently a check-then-use race, but the alternative is forking only to
  // discover the exec failed, which costs a process and an error path for the
  // common "not installed" case.
  [[nodiscard]] bool executableOnPath(const char* name);

} // namespace umbriel
