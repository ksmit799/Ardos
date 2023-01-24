#ifndef ARDOS_STATE_SERVER_H
#define ARDOS_STATE_SERVER_H

#include <memory>

#include <uvw.hpp>

namespace Ardos {

class StateServer {
public:
  StateServer();

private:
  std::shared_ptr<uvw::TCPHandle> _tcpHandle;
};

} // namespace Ardos

#endif // ARDOS_STATE_SERVER_H
