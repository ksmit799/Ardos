#ifndef ARDOS_LOGGER_H
#define ARDOS_LOGGER_H

#include <spdlog/spdlog.h>

namespace Ardos {

class Logger {
 public:
  static spdlog::level::level_enum LevelFromString(const std::string& level) {
    if (level == "trace") {
      return spdlog::level::trace;
    }
    if (level == "debug") {
      return spdlog::level::debug;
    }
    if (level == "info") {
      return spdlog::level::info;
    }
    if (level == "warning" || level == "warn") {
      return spdlog::level::warn;
    }
    if (level == "error") {
      return spdlog::level::err;
    }
    if (level == "none") {
      return spdlog::level::off;
    }

    spdlog::error("Invalid config log-level `{}`, defaulting to warn...",
                  level);
    return spdlog::level::warn;
  }
};

}  // namespace Ardos

#endif  // ARDOS_LOGGER_H
