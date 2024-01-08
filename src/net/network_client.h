#ifndef ARDOS_NETWORK_CLIENT_H
#define ARDOS_NETWORK_CLIENT_H

#include <memory>

#include <uvw.hpp>

#include "datagram.h"

namespace Ardos {

class NetworkClient {
public:
  explicit NetworkClient(const std::shared_ptr<uvw::tcp_handle> &socket);

protected:
  ~NetworkClient();

  [[nodiscard]] bool Disconnected() const;
  uvw::socket_address GetRemoteAddress();

  void Shutdown();

  virtual void HandleDisconnect(uv_errno_t code) = 0;
  virtual void HandleClientDatagram(const std::shared_ptr<Datagram> &dg) = 0;
  void SendDatagram(const std::shared_ptr<Datagram> &dg);

  bool _disconnected = false;

private:
  void HandleClose(uv_errno_t code);
  void HandleData(const std::unique_ptr<char[]> &data, size_t size);
  void ProcessBuffer();

  std::shared_ptr<uvw::tcp_handle> _socket;
  uvw::socket_address _remoteAddress;
  std::vector<uint8_t> _data_buf;

  bool _isWriting = false;
  bool _socketClosed = false;
};

} // namespace Ardos

#endif // ARDOS_NETWORK_CLIENT_H
