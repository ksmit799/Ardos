#include "ws_transport.h"

#include <spdlog/spdlog.h>

#include "../util/globals.h"

namespace Ardos {

namespace {

// Threaded through ws28 via Server::SetUserData / Client::SetUserData so
// our static callbacks can find the listener and per-connection objects
// without globals.

void OnWsClientConnected(ws28::Client* client, ws28::HTTPRequest&) {
  auto* server = client->GetServer();
  auto* listener = static_cast<WsTransportListener*>(server->GetUserData());
  if (!listener) {
    return;
  }

  auto conn = std::make_unique<WsTransportConnection>(client);
  // Stash the raw connection pointer so subsequent data/disconnect
  // callbacks can find it. Cleared in OnWsDisconnect/Close to prevent
  // use-after-free if a stale callback fires after teardown.
  client->SetUserData(conn.get());

  // Hand the connection to the factory. After this call returns, the
  // connection is owned by the handler (typically a ClientParticipant).
  if (listener->Factory()) {
    listener->Factory()(std::move(conn));
  } else {
    // No factory wired up; nothing to do but tear the client down.
    client->SetUserData(nullptr);
    client->Destroy();
  }
}

void OnWsClientDisconnected(ws28::Client* client) {
  auto* conn = static_cast<WsTransportConnection*>(client->GetUserData());
  if (!conn) {
    return;  // already torn down (e.g. via Close())
  }
  client->SetUserData(nullptr);
  conn->OnWsDisconnect();
}

void OnWsClientData(ws28::Client* client, char* data, size_t len,
                    int /*opcode*/) {
  auto* conn = static_cast<WsTransportConnection*>(client->GetUserData());
  if (!conn) {
    return;
  }
  conn->OnWsData(reinterpret_cast<const uint8_t*>(data), len);
}

}  // namespace

WsTransportConnection::WsTransportConnection(ws28::Client* client)
    : _client(client),
      _remoteEndpoint{client->GetIP() ? client->GetIP() : "", 0} {}

WsTransportConnection::~WsTransportConnection() {
  // If we still have a live ws28::Client pointer at destruction time
  // (i.e. Close() / OnWsDisconnect() didn't already null it out), tear
  // the underlying client down now. Detach our user-data pointer first
  // so the resulting disconnected callback no-ops.
  if (_client) {
    auto* c = _client;
    _client = nullptr;
    c->SetUserData(nullptr);
    c->Destroy();
  }
}

void WsTransportConnection::SetHandler(std::weak_ptr<ITransportHandler> h) {
  _handler = std::move(h);
}

void WsTransportConnection::Send(const uint8_t* data, size_t len,
                                 Reliability /*r*/) {
  // WS is always reliable; reliability hint is ignored. The third arg to
  // ws28::Client::Send is the opcode: 2 = binary frame, which matches our
  // datagram protocol.
  if (_closed || !_client) {
    return;
  }
  _client->Send(reinterpret_cast<const char*>(data), len, /*opCode=*/2);
}

void WsTransportConnection::Close() {
  if (_closed) {
    return;
  }
  _closed = true;
  if (_client) {
    auto* c = _client;
    _client = nullptr;
    c->SetUserData(nullptr);
    c->Destroy();
  }
}

TransportEndpoint WsTransportConnection::RemoteEndpoint() const {
  return _remoteEndpoint;
}

TransportEndpoint WsTransportConnection::LocalEndpoint() const {
  // ws28 doesn't surface the bound listen address per-client. Return an
  // empty endpoint -- callers that need a non-empty local address should
  // configure TCP or carry it via a config option.
  return {};
}

void WsTransportConnection::OnWsData(const uint8_t* data, size_t len) {
  if (auto handler = _handler.lock()) {
    handler->OnTransportMessage(data, len);
  }
}

void WsTransportConnection::OnWsDisconnect() {
  // The ws28::Client is being destroyed; drop our pointer to it so any
  // subsequent Send/Close calls become no-ops rather than dereferencing
  // freed memory.
  _client = nullptr;
  _closed = true;
  if (auto handler = _handler.lock()) {
    handler->OnTransportDisconnect();
  }
}

WsTransportListener::WsTransportListener()
    : _server(std::make_unique<ws28::Server>(g_loop->raw())) {}

void WsTransportListener::SetConnectionFactory(ConnectionFactory factory) {
  _factory = std::move(factory);
}

bool WsTransportListener::Listen(const std::string& host, int port) {
  // ws28's framed message size cap. Match the same cap the rest of the
  // protocol assumes (uint16 max for compatibility with TCP framing on
  // the producer side). +2 leaves room for an optional inline length
  // prefix if a client chooses to send one.
  _server->SetMaxMessageSize(0xFFFF + 2);

  // Disable Origin enforcement -- game clients aren't browsers and
  // don't carry meaningful Origin headers. (TLS isn't terminated here
  // anyway; reverse proxy handles cross-origin policy.)
  _server->SetCheckConnectionCallback(
      [](ws28::Client*, ws28::HTTPRequest&) { return true; });

  _server->SetClientConnectedCallback(&OnWsClientConnected);
  _server->SetClientDisconnectedCallback(&OnWsClientDisconnected);
  _server->SetClientDataCallback(&OnWsClientData);

  // Stash `this` so the static callbacks above can find the listener
  // via the ws28::Server they're handed.
  _server->SetUserData(this);

  if (!_server->Listen(port)) {
    spdlog::get("ca")->error("WebSocket transport failed to bind port {}",
                             port);
    return false;
  }

  spdlog::get("ca")->info("WebSocket transport listening on {}:{}", host, port);
  return true;
}

}  // namespace Ardos
