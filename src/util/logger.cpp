#include "logger.h"

#include <format>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace Ardos {

LogLevel Logger::_logLevel = LL_Warning;

void Logger::SetLogLevel(const LogLevel &level) { _logLevel = level; }

void Logger::SetLogLevel(const std::string_view level) {
  if (level.compare("verbose") == 0) {
    SetLogLevel(LL_Verbose);
  } else if (level.compare("info") == 0) {
    SetLogLevel(LL_Info);
  } else if (level.compare("warning") == 0 || level.compare("warn") == 0) {
    SetLogLevel(LL_Warning);
  } else if (level.compare("error") == 0) {
    SetLogLevel(LL_Error);
  } else if (level.compare("none") == 0) {
    SetLogLevel(LL_None);
  } else {
    Logger::Error(std::format(
        "Invalid config log-level `{}`, defaulting to warn...", level));
    SetLogLevel(LL_Warning);
  }
}

void Logger::Verbose(const std::string_view out) {
  if (_logLevel < LL_Verbose) {
    return;
  }

  std::cout << "[" << GetTime() << "] "
            << "[VERBOSE]: " << out << std::endl;
}

void Logger::Info(const std::string_view out) {
  if (_logLevel < LL_Info) {
    return;
  }

  std::cout << "[" << GetTime() << "] "
            << "[INFO]: " << out << std::endl;
}

void Logger::Warn(const std::string_view out) {
  if (_logLevel < LL_Warning) {
    return;
  }

  std::cout << "[" << GetTime() << "] "
            << "[WARNING]: " << out << std::endl;
}

void Logger::Error(const std::string_view out) {
  if (_logLevel < LL_Error) {
    return;
  }

  std::cout << "[" << GetTime() << "] "
            << "[ERROR]: " << out << std::endl;
}

std::string Logger::GetTime() {
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);

  std::ostringstream oss;
  oss << std::put_time(&tm, "%d-%m-%Y %H:%M:%S");
  return oss.str();
}

} // namespace Ardos