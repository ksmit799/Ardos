#ifndef ARDOS_STATE_CLIENT_H
#define ARDOS_STATE_CLIENT_H

#include <memory>

#include <uvw.hpp>

namespace Ardos {

class StateClient {
public:
  explicit StateClient(const std::shared_ptr<uvw::TCPHandle>& socket);
  ~StateClient();

private:
  std::shared_ptr<uvw::TCPHandle> _socket;
  uvw::Addr _remoteAddress;

  void HandleDisconnect(uv_errno_t errorCode);
};

} // namespace Ardos

#endif // ARDOS_STATE_CLIENT_H
