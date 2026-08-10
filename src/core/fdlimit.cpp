#include "core/fdlimit.h"

#include "core/log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <format>
#include <string_view>
#include <sys/resource.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {
  constexpr Logger kLog("fdlimit");

  rlimit gOriginalLimit{};
  bool gRaised = false;

  // Escalating thresholds, as a percentage of the soft limit.
  constexpr int kPressureThresholds[] = {25, 50, 75, 90};
  bool gThresholdReported[std::size(kPressureThresholds)] = {};

  [[nodiscard]] std::string rlimitValue(rlim_t value) {
    if (value == RLIM_INFINITY) {
      return "infinity";
    }
    return std::to_string(static_cast<unsigned long long>(value));
  }

  [[nodiscard]] bool isFdName(const char* name) {
    if (name == nullptr || name[0] == '\0') {
      return false;
    }
    for (const char* p = name; *p != '\0'; ++p) {
      if (*p < '0' || *p > '9') {
        return false;
      }
    }
    return true;
  }

  // Collapse the volatile part of a target so counts aggregate: every shm pool
  // becomes "memfd:wine-mapping" rather than 664 distinct inode paths.
  [[nodiscard]] std::string bucketTarget(std::string target) {
    for (std::string_view prefix : {"socket:", "pipe:", "anon_inode:"}) {
      if (target.starts_with(prefix)) {
        return std::string(prefix.substr(0, prefix.size() - 1));
      }
    }
    if (const std::size_t deleted = target.find(" (deleted)"); deleted != std::string::npos) {
      target.resize(deleted);
    }
    // "/memfd:wine-mapping" and "/dmabuf:1234-noctalia" keep their name, drop the pid.
    if (const std::size_t colon = target.find(':'); colon != std::string::npos) {
      const std::size_t dash = target.find('-', colon);
      if (dash != std::string::npos && target.find_first_not_of("0123456789", colon + 1) == dash) {
        target.erase(colon + 1, dash - colon);
      }
    }
    if (target.size() > 80) {
      target.resize(77);
      target += "...";
    }
    return target;
  }

  [[nodiscard]] std::string readFdTarget(const char* fdName) {
    const std::string path = std::string("/proc/self/fd/") + fdName;
    std::vector<char> buffer(512);
    while (true) {
      const ssize_t n = readlink(path.c_str(), buffer.data(), buffer.size() - 1);
      if (n < 0) {
        return "<unreadable>";
      }
      if (static_cast<std::size_t>(n) < buffer.size() - 1) {
        buffer[static_cast<std::size_t>(n)] = '\0';
        return std::string(buffer.data());
      }
      buffer.resize(buffer.size() * 2U);
      if (buffer.size() > 8192U) {
        return "<too long>";
      }
    }
  }

  [[nodiscard]] std::size_t countOpenFds() {
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
      return 0;
    }
    std::size_t count = 0;
    const int directoryFd = dirfd(dir);
    while (dirent* entry = readdir(dir)) {
      if (!isFdName(entry->d_name)) {
        continue;
      }
      if (std::atoi(entry->d_name) == directoryFd) {
        continue;
      }
      ++count;
    }
    closedir(dir);
    return count;
  }
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
    kLog.info("RLIMIT_NOFILE already at hard limit ({})", rlimitValue(limit.rlim_cur));
    return;
  }

  const rlim_t previous = limit.rlim_cur;
  limit.rlim_cur = limit.rlim_max;
  if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
    kLog.warn("failed to raise RLIMIT_NOFILE from {}: {}", rlimitValue(previous), std::strerror(errno));
    return;
  }

  gRaised = true;
  kLog.info("raised RLIMIT_NOFILE {} -> {}", rlimitValue(previous), rlimitValue(limit.rlim_max));
}

void restoreFileDescriptorLimit() {
  if (!gRaised) {
    return;
  }
  // Runs between fork() and exec(): must stay async-signal-safe, so no logging
  // on failure — the child is about to be replaced anyway.
  (void)setrlimit(RLIMIT_NOFILE, &gOriginalLimit);
}

std::string describeOpenFileDescriptors(std::size_t maxTargets) {
  rlimit limit{};
  const std::string limitText = getrlimit(RLIMIT_NOFILE, &limit) == 0
      ? std::format("rlimit_nofile={}/{}", rlimitValue(limit.rlim_cur), rlimitValue(limit.rlim_max))
      : "rlimit_nofile=unavailable";

  DIR* dir = opendir("/proc/self/fd");
  if (dir == nullptr) {
    return std::format("open_fds=unavailable ({}), {}", std::strerror(errno), limitText);
  }

  std::size_t count = 0;
  std::unordered_map<std::string, std::size_t> targetCounts;
  const int directoryFd = dirfd(dir);
  while (dirent* entry = readdir(dir)) {
    if (!isFdName(entry->d_name) || std::atoi(entry->d_name) == directoryFd) {
      continue;
    }
    ++count;
    ++targetCounts[bucketTarget(readFdTarget(entry->d_name))];
  }
  closedir(dir);

  std::vector<std::pair<std::string, std::size_t>> targets(targetCounts.begin(), targetCounts.end());
  std::ranges::sort(targets, [](const auto& lhs, const auto& rhs) {
    return lhs.second != rhs.second ? lhs.second > rhs.second : lhs.first < rhs.first;
  });

  std::string out = std::format("open_fds={}, {}", count, limitText);
  if (!targets.empty() && maxTargets > 0) {
    out += ", top_fd_targets=[";
    for (std::size_t i = 0; i < std::min(maxTargets, targets.size()); ++i) {
      if (i != 0) {
        out += ", ";
      }
      out += std::format("{}={}", targets[i].first, targets[i].second);
    }
    out += "]";
  }
  return out;
}

void checkFileDescriptorPressure() {
  rlimit limit{};
  if (getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur == 0 || limit.rlim_cur == RLIM_INFINITY) {
    return;
  }

  const std::size_t open = countOpenFds();
  const auto percent = static_cast<int>((open * 100U) / limit.rlim_cur);

  // Report only the highest newly-crossed threshold, so jumping straight from
  // idle to 90% logs one line rather than four.
  bool crossed = false;
  for (std::size_t i = 0; i < std::size(kPressureThresholds); ++i) {
    if (percent >= kPressureThresholds[i]) {
      crossed = crossed || !gThresholdReported[i];
      gThresholdReported[i] = true;
    } else {
      gThresholdReported[i] = false; // re-arm once pressure drops back off
    }
  }

  if (crossed) {
    kLog.warn("file descriptor usage at {}% of soft limit: {}", percent, describeOpenFileDescriptors());
  }
}
