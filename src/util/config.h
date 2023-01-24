#ifndef ARDOS_CONFIG_H
#define ARDOS_CONFIG_H

#include <yaml-cpp/yaml.h>

namespace Ardos {

class Config {
public:
  static Config *Instance();

  void LoadConfig(const std::string &name);

  std::string GetString(const std::string &key, const std::string &defVal = "");
  YAML::Node GetNode(const std::string &key);
  bool GetBool(const std::string &key, const bool &defVal = false);

protected:
  Config() = default;
  ~Config() = default;

  static Config *_instance;

  YAML::Node _config;
};

} // namespace Ardos

#endif // ARDOS_CONFIG_H
