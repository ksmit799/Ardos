#ifndef ARDOS_CLIENT_AGENT_H
#define ARDOS_CLIENT_AGENT_H

#include <memory>

#include <uvw.hpp>

namespace Ardos {

class ClientAgent {
public:
  ClientAgent();

private:
  std::shared_ptr<uvw::TCPHandle> _tcpHandle;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_AGENT_H
