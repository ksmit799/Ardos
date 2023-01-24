#include "config.h"
#include "logger.h"

#include <format>
#include <fstream>

namespace Ardos {

Config *Config::_instance = nullptr;

Config *Config::Instance() {
  if (_instance == nullptr) {
    _instance = new Config();
  }

  return _instance;
}

void Config::LoadConfig(const std::string &name) {
  std::ifstream file(name.c_str());
  if (!file.is_open()) {
    Logger::Error(
        std::format("Failed to open config file `{}`. Does it exist?", name));
    exit(1);
  }

  _config = YAML::Load(file);
}

std::string Config::GetString(const std::string &key,
                              const std::string &defVal) {
  auto val = _config[key].as<std::string>();
  return !val.empty() ? val : defVal;
}

YAML::Node Config::GetNode(const std::string &key) {
  return _config[key];
}

bool Config::GetBool(const std::string &key, const bool &defVal) {
  if (auto param = _config[key]) {
    return param.as<bool>();
  }

  return defVal;
}

} // namespace Ardos
