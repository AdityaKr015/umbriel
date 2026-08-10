#include "core/fdlimit.h"

#include "core/log.h"

#include <cerrno>
#include <cstring>
#include <sys/resource.h>

namespace {
  constexpr Logger kLog("fdlimit");

  rlimit gOriginalLimit{};
  bool gRaised = false;
} // namespace

void raiseFileDescriptorLimit() {
  if (gRaised) {
    return;
  }

  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.warn("getrlimit(RLIMIT_NOFILE) failed: {}", std::strerror(errno));
    return;
  }

  gOriginalLimit = limit;
  if (limit.rlim_cur >= limit.rlim_max) {
    gRaised = true; // Nothing to raise, but children still inherit the right value.
    return;
  }

  const rlim_t previous = limit.rlim_cur;
  limit.rlim_cur = limit.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.warn("failed to raise RLIMIT_NOFILE from {}: {}", previous, std::strerror(errno));
    return;
  }

  gRaised = true;
  kLog.info("raised RLIMIT_NOFILE {} -> {}", previous, limit.rlim_max);
}

void restoreFileDescriptorLimit() {
  if (!gRaised) {
    return;
  }
  // Runs between fork() and exec(): must stay async-signal-safe, so no logging
  // on failure — the child is about to be replaced anyway.
  (void)setrlimit(RLIMIT_NOFILE, &gOriginalLimit);
}
