#ifndef ARDOS_TRANSPORT_H
#define ARDOS_TRANSPORT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace Ardos {

// Reliability hint for transports that support multiple lanes (currently
// only meaningful for GameNetworkingSockets in the future). TCP/WS always
// deliver reliably and ignore the flag.
enum class Reliability : std::uint8_t {
  Reliable,
  Unreliable,
};

// Network endpoint (ip + port). Transports that don't expose a meaningful
// port (e.g. ws28 keeps only the peer IP) leave port=0.
struct TransportEndpoint {
  std::string ip;
  std::uint16_t port = 0;
};

// Receives events from a transport. Implemented by ClientParticipant.
//
// The connection holds a weak_ptr to its handler so a destroyed handler
// doesn't strand callbacks on freed memory -- if the handler has been
// released, the connection's late events simply no-op.
class ITransportHandler {
 public:
  virtual ~ITransportHandler() = default;

  // One complete protocol datagram has been received from the peer. The
  // transport has already stripped any transport-specific framing (TCP
  // length prefix, WS frame header, etc.) -- the payload is the raw
  // protocol message body.
  virtual void OnTransportMessage(const uint8_t* data, size_t len) = 0;

  // The peer closed the connection or the transport errored out. Called
  // at most once per connection. The handler should treat this as the
  // signal to release any resources tied to the connection.
  virtual void OnTransportDisconnect() = 0;
};

// Per-connection transport handle owned by the per-connection handler
// (typically ClientParticipant). Sending and closing are routed through
// this; transport-specific framing/encoding lives in the implementation.
class ITransportConnection {
 public:
  virtual ~ITransportConnection() = default;

  // Bind the handler that receives this connection's events. Called once
  // immediately after construction, from the handler's Init() (so that
  // shared_from_this is valid). Subsequent OnTransportMessage /
  // OnTransportDisconnect callbacks lock this weak_ptr; if it has
  // expired, the events are dropped.
  virtual void SetHandler(std::weak_ptr<ITransportHandler> handler) = 0;

  // Send a single protocol datagram. The transport applies its own
  // framing (length prefix for TCP, WS frame for WS). The reliability
  // hint is passed through to transports that distinguish lanes; others
  // ignore it.
  virtual void Send(const uint8_t* data, size_t len,
                    Reliability r = Reliability::Reliable) = 0;

  // Close the connection. Idempotent. Triggers OnTransportDisconnect on
  // the handler (asynchronously, after the underlying socket has been
  // cleanly closed).
  virtual void Close() = 0;

  // Remote and local endpoints. Used for logging and for
  // CLIENTAGENT_GET_NETWORK_ADDRESS_RESP. Transports without port info
  // (WS for now) return port=0.
  [[nodiscard]] virtual TransportEndpoint RemoteEndpoint() const = 0;
  [[nodiscard]] virtual TransportEndpoint LocalEndpoint() const = 0;
};

// Accepts incoming connections on a host:port and dispatches each to a
// factory that produces the per-connection handler. Concrete impls:
// TcpTransportListener (libuv TCP) and WsTransportListener (ws28). The
// listener owns the underlying socket / WS server; its lifetime is
// bounded by the ClientAgent that created it.
class ITransportListener {
 public:
  // Invoked on each accepted connection. Takes ownership of the
  // connection; the listener retains no reference after the call.
  using ConnectionFactory =
      std::function<void(std::unique_ptr<ITransportConnection>)>;

  virtual ~ITransportListener() = default;

  virtual void SetConnectionFactory(ConnectionFactory factory) = 0;

  // Start accepting connections. Returns true on success. On failure,
  // the listener should log the reason and return false; the caller
  // typically logs+exits since the role can't function without its
  // listen socket.
  virtual bool Listen(const std::string& host, int port) = 0;
};

}  // namespace Ardos

#endif  // ARDOS_TRANSPORT_H
