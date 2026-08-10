#pragma once

#include <cstddef>
#include <string>

// A compositor holds a file descriptor for every client connection, every
// wl_shm pool, every dmabuf plane and every explicit-sync fence it dups. The
// 1024 soft limit inherited from the session manager is small enough that a
// single client churning shm pools can exhaust it, after which
// eglDupNativeFenceFDANDROID fails on every frame and the session wedges.
// Raise the soft limit to the hard limit at startup, and put it back before
// exec'ing children so they keep the conventional 1024 (a large soft limit
// breaks select() and slows down anything that loops over the fd table).

void raiseFileDescriptorLimit();
void restoreFileDescriptorLimit();

// "open_fds=712, rlimit_nofile=1048576/1048576, top_fd_targets=[memfd=664, ...]"
// Buckets by target kind so a runaway client is named rather than guessed at.
[[nodiscard]] std::string describeOpenFileDescriptors(std::size_t maxTargets = 8);

// Samples the fd table and logs the description above the first time usage
// crosses each of 25/50/75/90% of the soft limit. Silent otherwise, so it is
// safe to call on a timer. Thresholds re-arm if usage falls back below them.
void checkFileDescriptorPressure();
