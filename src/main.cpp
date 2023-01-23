#include <iostream>

#include <yaml-cpp/yaml.h>

int main() {
  YAML::Node config = YAML::LoadFile("config.yaml");

  if (config["lastLogin"]) {
    std::cout << "Last logged in: " << config["lastLogin"].as<std::string>() << "\n";
  }

  return 0;
}
