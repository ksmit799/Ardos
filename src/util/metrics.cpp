#include "metrics.h"

#include <prometheus/exposer.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "config.h"
#include "logger.h"

namespace Ardos {

Metrics *Metrics::_instance = nullptr;

Metrics *Metrics::Instance() {
  if (_instance == nullptr) {
    _instance = new Metrics();
  }

  return _instance;
}

Metrics::Metrics() {
  // Do we want to run Prometheus on this cluster?
  _wantMetrics = Config::Instance()->GetBool("want-metrics");
  if (!_wantMetrics) {
    return;
  }

  spdlog::info("Starting Metrics component...");

  auto config = Config::Instance()->GetNode("metrics");

  // Log configuration.
  spdlog::stdout_color_mt("metrics");
  if (auto logLevel = config["log-level"]) {
    spdlog::get("metrics")->set_level(
        Logger::LevelFromString(logLevel.as<std::string>()));
  }

  // Listen configuration.
  if (auto hostParam = config["host"]) {
    _host = hostParam.as<std::string>();
  }
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }

  // Create an HTTP server on the configured host/port.
  _exposer =
      std::make_unique<prometheus::Exposer>(std::format("{}:{}", _host, _port));

  // Ask the exposer to scrape the registry on incoming HTTP requests.
  _registry = std::make_shared<prometheus::Registry>();
  _exposer->RegisterCollectable(_registry);

  spdlog::get("metrics")->info("Listening on {}:{}", _host, _port);
}

bool Metrics::WantMetrics() const { return _wantMetrics; }

std::shared_ptr<prometheus::Registry> Metrics::GetRegistry() {
  return _registry;
}

} // namespace Ardos
