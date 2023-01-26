#ifndef ARDOS_STATE_SERVER_H
#define ARDOS_STATE_SERVER_H

#include <memory>

#include <uvw.hpp>

#include "state_client.h"

namespace Ardos {

class StateServer {
public:
  StateServer();

private:
  std::shared_ptr<uvw::TCPHandle> _tcpHandle;
  std::vector<std::unique_ptr<StateClient>> _clients;
};

} // namespace Ardos

#endif // ARDOS_STATE_SERVER_H
