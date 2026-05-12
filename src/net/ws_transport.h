#ifndef ARDOS_WS_TRANSPORT_H
#define ARDOS_WS_TRANSPORT_H

#include <ws28/Server.h>

#include <memory>
#include <string>

#include "transport.h"

namespace Ardos {

// WebSocket connection wrapping a ws28::Client. ws28 owns the Client; we
// hold a raw pointer nulled on disconnect/Close so subsequent Send/Close
// calls no-op. Each datagram is sent as one binary WS frame -- no length
// prefix on the wire, the frame is the boundary.
class WsTransportConnection final : public ITransportConnection {
 public:
  explicit WsTransportConnection(ws28::Client* client);
  ~WsTransportConnection() override;

  void SetHandler(std::weak_ptr<ITransportHandler> handler) override;
  void Send(const uint8_t* data, size_t len,
            Reliability r = Reliability::Reliable) override;
  void Close() override;
  [[nodiscard]] TransportEndpoint RemoteEndpoint() const override;
  [[nodiscard]] TransportEndpoint LocalEndpoint() const override;

  // Called by the listener's static ws28 callbacks (via the user-data
  // pointer stashed on the ws28::Client at construction).
  void OnWsData(const uint8_t* data, size_t len);
  void OnWsDisconnect();

 private:
  ws28::Client* _client;  // ws28::Server owns; nulled on disconnect/close
  std::weak_ptr<ITransportHandler> _handler;
  // ws28 surfaces only the peer IP, not port. Port is therefore 0; the
  // local endpoint is similarly empty (ws28 doesn't expose the listen
  // socket's bound port through its Client API). The GET_NETWORK_ADDRESS
  // response will carry the IP correctly and zeroes for the ports.
  TransportEndpoint _remoteEndpoint;
  bool _closed = false;
};

// ws28-backed WebSocket listener. Wraps each inbound ws28::Client in a
// WsTransportConnection and hands it to the configured factory.
class WsTransportListener final : public ITransportListener {
 public:
  WsTransportListener();
  ~WsTransportListener() override = default;

  void SetConnectionFactory(ConnectionFactory factory) override;
  bool Listen(const std::string& host, int port) override;

  // Accessor for the static ws28 callbacks (they receive a ws28::Server*
  // and reach back into us via Server::GetUserData()).
  [[nodiscard]] const ConnectionFactory& Factory() const { return _factory; }

 private:
  std::unique_ptr<ws28::Server> _server;
  ConnectionFactory _factory;
};

}  // namespace Ardos

#endif  // ARDOS_WS_TRANSPORT_H
