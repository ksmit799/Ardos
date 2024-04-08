#ifndef ARDOS_LOGGER_H
#define ARDOS_LOGGER_H

#include <spdlog/spdlog.h>

namespace Ardos {

class Logger {
public:
  static spdlog::level::level_enum LevelFromString(const std::string &level) {
    if (level == "debug" || level == "verbose") {
      return spdlog::level::debug;
    } else if (level == "info") {
      return spdlog::level::info;
    } else if (level == "warning" || level == "warn") {
      return spdlog::level::warn;
    } else if (level == "error") {
      return spdlog::level::err;
    } else if (level == "none") {
      return spdlog::level::off;
    } else {
      spdlog::error("Invalid config log-level `{}`, defaulting to warn...",
                    level);
      return spdlog::level::warn;
    }
  }
};

} // namespace Ardos

#endif // ARDOS_LOGGER_H
