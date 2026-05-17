#ifndef ARDOS_TCP_TRANSPORT_H
#define ARDOS_TCP_TRANSPORT_H

#include <deque>
#include <memory>
#include <string>
#include <uvw.hpp>
#include <vector>

#include "transport.h"

namespace Ardos {

// libuv-backed TCP connection. Frames protocol datagrams over the byte
// stream as [uint16 LE length][payload].
class TcpTransportConnection final : public ITransportConnection {
 public:
  explicit TcpTransportConnection(std::shared_ptr<uvw::tcp_handle> socket);
  ~TcpTransportConnection() override;

  void SetHandler(std::weak_ptr<ITransportHandler> handler) override;
  void Send(const uint8_t* data, size_t len,
            Reliability r = Reliability::Reliable) override;
  void Close() override;
  [[nodiscard]] TransportEndpoint RemoteEndpoint() const override;
  [[nodiscard]] TransportEndpoint LocalEndpoint() const override;

 private:
  void HandleClose(int err);
  // NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> from uvw read
  void HandleData(const std::unique_ptr<char[]>& data, size_t size);
  void ProcessBuffer();
  void DeliverMessage(const uint8_t* data, size_t len);
  // Issues the next queued write, if any, when no write is in flight.
  void PumpWrite();

  std::shared_ptr<uvw::tcp_handle> _socket;
  std::weak_ptr<ITransportHandler> _handler;
  TransportEndpoint _remoteEndpoint;
  TransportEndpoint _localEndpoint;

  // Accumulator for partial datagrams across uvw data_events.
  std::vector<uint8_t> _readBuffer;

  // Application-level write queue bounded by kHighWaterBytes.
  struct PendingWrite {
    // NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> for uvw
    std::unique_ptr<char[]> buf;
    size_t size;
  };
  std::deque<PendingWrite> _writeQueue;
  size_t _queuedBytes = 0;
  static constexpr size_t kHighWaterBytes = 4 * 1024 * 1024;  // 4 MiB

  bool _closed = false;
  bool _isWriting = false;
  bool _socketClosed = false;

  // Captured by every uvw event lambda; flipped false in the destructor
  // so late-firing callbacks (uvw close() is async) no-op instead of
  // accessing freed members on `this`.
  std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

// Accepts inbound TCP connections via libuv. On accept, builds a
// TcpTransportConnection and hands it to the configured factory (which
// is expected to construct and Init() a ClientParticipant around it).
class TcpTransportListener final : public ITransportListener {
 public:
  TcpTransportListener();
  ~TcpTransportListener() override = default;

  void SetConnectionFactory(ConnectionFactory factory) override;
  bool Listen(const std::string& host, int port) override;

 private:
  std::shared_ptr<uvw::tcp_handle> _listenHandle;
  ConnectionFactory _factory;
};

}  // namespace Ardos

#endif  // ARDOS_TCP_TRANSPORT_H
