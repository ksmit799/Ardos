#ifndef ARDOS_LOGGER_H
#define ARDOS_LOGGER_H

#include <string>

namespace Ardos {

enum LogLevel {
  LL_None,
  LL_Error,
  LL_Warning,
  LL_Info,
  LL_Verbose,
};

class Logger {
public:
  static void SetLogLevel(const LogLevel &level);
  static void SetLogLevel(std::string_view level);

  static void Verbose(std::string_view message);
  static void Info(std::string_view message);
  static void Warn(std::string_view message);
  static void Error(std::string_view message);

protected:
  Logger() = default;
  ~Logger() = default;

  static LogLevel _logLevel;

  static std::string GetTime();
};

} // namespace Ardos

#endif // ARDOS_LOGGER_H
