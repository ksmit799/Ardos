#ifndef ARDOS_METRICS_H
#define ARDOS_METRICS_H

#include <memory>
#include <string>

#include <prometheus/exposer.h>
#include <prometheus/registry.h>

namespace Ardos {

class Metrics {
public:
  static Metrics *Instance();

  [[nodiscard]] bool WantMetrics() const;
  std::shared_ptr<prometheus::Registry> GetRegistry();

private:
  Metrics();

  static Metrics *_instance;

  bool _wantMetrics = true;
  std::string _host = "127.0.0.1";
  int _port = 9985;

  std::unique_ptr<prometheus::Exposer> _exposer;
  std::shared_ptr<prometheus::Registry> _registry;
};

} // namespace Ardos

#endif // ARDOS_METRICS_H
