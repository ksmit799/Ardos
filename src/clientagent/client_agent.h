#ifndef ARDOS_CLIENT_AGENT_H
#define ARDOS_CLIENT_AGENT_H

#include <memory>

#include <uvw.hpp>

namespace Ardos {

class ClientAgent {
public:
  ClientAgent();

private:
  std::shared_ptr<uvw::TCPHandle> _listenHandle;

  uint64_t _channelsMin;
  uint64_t _channelsMax;

  std::string _host = "127.0.0.1";
  int _port = 6667;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_AGENT_H
