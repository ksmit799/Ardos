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
  static void SetLogLevel(const std::string& level);

  static void Verbose(const std::string &message);
  static void Info(const std::string &message);
  static void Warn(const std::string &message);
  static void Error(const std::string &message);

protected:
  Logger() = default;
  ~Logger() = default;

  static LogLevel _logLevel;

  static std::string GetTime();
};

} // namespace Ardos

#endif // ARDOS_LOGGER_H
