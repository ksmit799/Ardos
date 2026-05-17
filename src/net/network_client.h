#ifndef ARDOS_NETWORK_CLIENT_H
#define ARDOS_NETWORK_CLIENT_H

#include <deque>
#include <memory>
#include <uvw.hpp>

#include "datagram.h"

namespace Ardos {

class NetworkClient {
 public:
  explicit NetworkClient(const std::shared_ptr<uvw::tcp_handle>& socket);

  [[nodiscard]] uvw::socket_address GetRemoteAddress() const;
  [[nodiscard]] uvw::socket_address GetLocalAddress() const;

 protected:
  ~NetworkClient();

  [[nodiscard]] bool Disconnected() const;

  // Virtual so the high-water drop in SendDatagram dispatches to subclass
  // teardown (channel unsubscribes, post-removes, etc).
  virtual void Shutdown();

  virtual void HandleDisconnect(uv_errno_t code) = 0;
  virtual void HandleClientDatagram(const std::shared_ptr<Datagram>& dg) = 0;
  void SendDatagram(const std::shared_ptr<Datagram>& dg);

  bool _disconnected = false;

 private:
  void HandleClose(uv_errno_t code);
  // NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> from uvw read
  void HandleData(const std::unique_ptr<char[]>& data, size_t size);
  void ProcessBuffer();
  // Issues the next queued write, if any, when no write is in flight.
  void PumpWrite();

  std::shared_ptr<uvw::tcp_handle> _socket;
  uvw::socket_address _remoteAddress;
  uvw::socket_address _localAddress;
  std::vector<uint8_t> _data_buf;

  // Application-level write queue bounded by kHighWaterBytes.
  struct PendingWrite {
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> for uvw
    std::unique_ptr<char[]> buf;
    size_t size;
  };
  std::deque<PendingWrite> _writeQueue;
  size_t _queuedBytes = 0;
  static constexpr size_t kHighWaterBytes = size_t{4} * 1024 * 1024;  // 4 MiB

  bool _isWriting = false;
  bool _socketClosed = false;

  // Captured by every socket-event lambda. Set false in Shutdown so a
  // late-firing event after `this` is destroyed becomes a no-op.
  std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

}  // namespace Ardos

#endif  // ARDOS_NETWORK_CLIENT_H
