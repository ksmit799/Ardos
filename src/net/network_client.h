#ifndef ARDOS_NETWORK_CLIENT_H
#define ARDOS_NETWORK_CLIENT_H

#include <memory>

#include <uvw.hpp>

#include "datagram.h"

namespace Ardos {

class NetworkClient {
public:
  explicit NetworkClient(const std::shared_ptr<uvw::TCPHandle> &socket);
  ~NetworkClient();

protected:
  [[nodiscard]] bool Disconnected() const;
  uvw::Addr GetRemoteAddress();

  void Shutdown();

  virtual void HandleDisconnect(uv_errno_t code) = 0;
  virtual void HandleClientDatagram(const std::shared_ptr<Datagram> &dg) = 0;
  void SendDatagram(const std::shared_ptr<Datagram> &dg);

private:
  void HandleClose(uv_errno_t code);
  void HandleData(const std::unique_ptr<char[]> &data, size_t size);
  void ProcessBuffer();

  std::shared_ptr<uvw::TCPHandle> _socket;
  uvw::Addr _remoteAddress;
  std::vector<uint8_t> _data_buf;
  bool _disconnected = false;
};

} // namespace Ardos

#endif // ARDOS_NETWORK_CLIENT_H
