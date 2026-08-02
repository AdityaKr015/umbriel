#include "core/log.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace {

#ifdef NDEBUG
  LogLevel gMinLevel = LogLevel::Info;
#else
  LogLevel gMinLevel = LogLevel::Debug;
#endif

  FILE* gLogFile = nullptr;
  std::mutex gLogMutex;
  std::string gLogPath;
  bool gRegisteredExitFlush = false;

  const char* levelTagAnsi(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "\033[36mDBG\033[0m";
    case LogLevel::Info:
      return "\033[32mINF\033[0m";
    case LogLevel::Warn:
      return "\033[33mWRN\033[0m";
    case LogLevel::Error:
      return "\033[31mERR\033[0m";
    }
    return "???";
  }

  const char* levelTagPlain(LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
      return "DBG";
    case LogLevel::Info:
      return "INF";
    case LogLevel::Warn:
      return "WRN";
    case LogLevel::Error:
      return "ERR";
    }
    return "???";
  }

  void flushLogFileUnlocked() {
    if (gLogFile != nullptr) {
      std::fflush(gLogFile);
    }
  }

  void flushLogFileAtExit() {
    std::scoped_lock lock(gLogMutex);
    flushLogFileUnlocked();
  }

  void writeLine(FILE* stream, std::string_view prefix, std::string_view msg) {
    if (stream == nullptr) {
      return;
    }
    if (!prefix.empty()) {
      std::fwrite(prefix.data(), 1, prefix.size(), stream);
    }
    if (!msg.empty()) {
      std::fwrite(msg.data(), 1, msg.size(), stream);
    }
    std::fputc('\n', stream);
  }

  std::string consolePrefix(const std::tm& tm, long msec, LogLevel level, const char* section) {
    char buffer[96];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%02d:%02d:%02d.%03ld [%s]", tm.tm_hour, tm.tm_min, tm.tm_sec, msec, levelTagAnsi(level)
    );
    std::string prefix(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
    if (section != nullptr && section[0] != '\0') {
      prefix += " [\033[34m";
      prefix += section;
      prefix += "\033[0m]";
    }
    prefix += ' ';
    return prefix;
  }

  std::string filePrefix(const std::tm& tm, long msec, LogLevel level, const char* section) {
    char buffer[128];
    const int length = std::snprintf(
        buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%03ld [%s]", tm.tm_year + 1900, tm.tm_mon + 1,
        tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, msec, levelTagPlain(level)
    );
    std::string prefix(buffer, length > 0 ? static_cast<std::size_t>(length) : 0);
    if (section != nullptr && section[0] != '\0') {
      prefix += " [";
      prefix += section;
      prefix += ']';
    }
    prefix += ' ';
    return prefix;
  }

} // namespace

void initLogFile() {
  const char* cacheHome = std::getenv("XDG_CACHE_HOME");
  const char* home = std::getenv("HOME");

  std::string dir;
  if (cacheHome != nullptr && cacheHome[0] != '\0') {
    dir = std::string(cacheHome) + "/umbriel";
  } else if (home != nullptr && home[0] != '\0') {
    dir = std::string(home) + "/.cache/umbriel";
  } else {
    return;
  }

  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return;
  }

  std::scoped_lock lock(gLogMutex);
  if (gLogFile != nullptr) {
    std::fclose(gLogFile);
    gLogFile = nullptr;
  }

  gLogPath = dir + "/umbriel.log";
  gLogFile = std::fopen(gLogPath.c_str(), "a");
  if (gLogFile != nullptr && !gRegisteredExitFlush) {
    (void)std::atexit(flushLogFileAtExit);
    gRegisteredExitFlush = true;
  }
}

namespace detail {

  void logMessage(LogLevel level, const char* section, std::string_view msg) {
    std::timespec ts{};
    std::timespec_get(&ts, TIME_UTC);
    std::tm tm{};
    localtime_r(&ts.tv_sec, &tm);
    const long msec = ts.tv_nsec / 1'000'000;

    std::scoped_lock lock(gLogMutex);

    if (level >= gMinLevel) {
      writeLine(stderr, consolePrefix(tm, msec, level, section), msg);
    }

    if (gLogFile != nullptr) {
      writeLine(gLogFile, filePrefix(tm, msec, level, section), msg);
      if (level >= LogLevel::Warn) {
        flushLogFileUnlocked();
      }
    }
  }

} // namespace detail
