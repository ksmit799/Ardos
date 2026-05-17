#include "tcp_transport.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <limits>

#include "../util/globals.h"

namespace Ardos {

TcpTransportConnection::TcpTransportConnection(
    std::shared_ptr<uvw::tcp_handle> socket)
    : _socket(std::move(socket)) {
  _socket->no_delay(true);
  _socket->keep_alive(true, uvw::tcp_handle::time{60});

  auto remote = _socket->peer();
  auto local = _socket->sock();
  _remoteEndpoint = {.ip = remote.ip,
                     .port = static_cast<uint16_t>(remote.port)};
  _localEndpoint = {.ip = local.ip, .port = static_cast<uint16_t>(local.port)};

  // Wire libuv events. Every lambda captures `_alive` so a late-firing
  // event after this connection has been closed becomes a no-op rather
  // than dereferencing freed memory (uvw close() is async).
  _socket->on<uvw::error_event>(
      [this, alive = _alive](const uvw::error_event& event, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        HandleClose(event.code());
      });

  _socket->on<uvw::end_event>(
      [this, alive = _alive](const uvw::end_event&, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        HandleClose(UV_EOF);
      });

  _socket->on<uvw::close_event>(
      [this, alive = _alive](const uvw::close_event&, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        HandleClose(UV_EOF);
      });

  _socket->on<uvw::data_event>(
      [this, alive = _alive](const uvw::data_event& event, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        HandleData(event.data, event.length);
      });

  _socket->on<uvw::write_event>(
      [this, alive = _alive](const uvw::write_event&, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        _isWriting = false;
        if (_closed) {
          // Drop anything still queued — the connection is going away.
          _writeQueue.clear();
          _queuedBytes = 0;
          if (!_socketClosed) {
            _socket->close();
            _socketClosed = true;
          }
          return;
        }
        PumpWrite();
      });

  _socket->read();
}

TcpTransportConnection::~TcpTransportConnection() {
  Close();
  *_alive = false;
}

void TcpTransportConnection::SetHandler(std::weak_ptr<ITransportHandler> h) {
  _handler = std::move(h);
}

void TcpTransportConnection::Send(const uint8_t* data, size_t len,
                                  Reliability /*r*/) {
  // TCP is always reliable; the hint is ignored.
  if (_closed || _socket == nullptr) {
    return;
  }

  // Framing prefix is uint16; larger payloads would wrap and corrupt.
  if (len > std::numeric_limits<uint16_t>::max()) {
    spdlog::get("ca")->error(
        "TCP transport refusing oversized datagram ({}B > {}B max)", len,
        std::numeric_limits<uint16_t>::max());
    return;
  }

  const size_t sendSize = sizeof(uint16_t) + len;

  if (_queuedBytes + sendSize > kHighWaterBytes) {
    spdlog::get("ca")->warn(
        "TCP transport: client {}:{} exceeded {}B write backlog; disconnecting",
        _remoteEndpoint.ip, _remoteEndpoint.port, kHighWaterBytes);
    Close();
    return;
  }

  // NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> for uvw write
  auto sendBuffer = std::unique_ptr<char[]>(new char[sendSize]);
  const auto dgSize = static_cast<uint16_t>(len);
  std::memcpy(sendBuffer.get(), &dgSize, sizeof(uint16_t));
  std::memcpy(sendBuffer.get() + sizeof(uint16_t), data, len);

  _writeQueue.push_back({std::move(sendBuffer), sendSize});
  _queuedBytes += sendSize;

  PumpWrite();
}

void TcpTransportConnection::PumpWrite() {
  if (_isWriting || _writeQueue.empty() || _socketClosed) {
    return;
  }
  auto front = std::move(_writeQueue.front());
  _writeQueue.pop_front();
  _queuedBytes -= front.size;
  _isWriting = true;
  _socket->write(std::move(front.buf), front.size);
}

void TcpTransportConnection::Close() {
  if (_closed) {
    return;
  }
  _closed = true;

  // Abandon anything queued; we're not going to flush it.
  _writeQueue.clear();
  _queuedBytes = 0;

  // *_alive flips false only in the destructor; an in-flight write still
  // needs the write_event lambda to run and close the socket.
  if (!_isWriting && !_socketClosed) {
    _socket->close();
    _socketClosed = true;
  }
}

TransportEndpoint TcpTransportConnection::RemoteEndpoint() const {
  return _remoteEndpoint;
}

TransportEndpoint TcpTransportConnection::LocalEndpoint() const {
  return _localEndpoint;
}

void TcpTransportConnection::HandleClose(int /*err*/) {
  if (_closed) {
    return;
  }
  _closed = true;
  _socketClosed = true;

  if (auto handler = _handler.lock()) {
    handler->OnTransportDisconnect();
  }
}

// NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> from uvw read
void TcpTransportConnection::HandleData(const std::unique_ptr<char[]>& data,
                                        size_t size) {
  // Fast path: a single complete datagram in this chunk and no
  // accumulator state. Avoids the buffer copy.
  if (_readBuffer.empty() && size >= sizeof(uint16_t)) {
    uint16_t dgSize;
    std::memcpy(&dgSize, data.get(), sizeof(dgSize));
    if (dgSize == size - sizeof(uint16_t)) {
      DeliverMessage(
          reinterpret_cast<const uint8_t*>(data.get() + sizeof(uint16_t)),
          dgSize);
      return;
    }
  }

  _readBuffer.insert(_readBuffer.end(), data.get(), data.get() + size);
  ProcessBuffer();
}

void TcpTransportConnection::ProcessBuffer() {
  while (_readBuffer.size() > sizeof(uint16_t)) {
    uint16_t dgSize;
    std::memcpy(&dgSize, _readBuffer.data(), sizeof(dgSize));
    if (_readBuffer.size() < dgSize + sizeof(uint16_t)) {
      return;  // partial; wait for more bytes
    }

    DeliverMessage(_readBuffer.data() + sizeof(uint16_t), dgSize);
    _readBuffer.erase(_readBuffer.begin(),
                      _readBuffer.begin() + sizeof(uint16_t) + dgSize);
  }
}

void TcpTransportConnection::DeliverMessage(const uint8_t* data, size_t len) {
  if (auto handler = _handler.lock()) {
    handler->OnTransportMessage(data, len);
  }
}

TcpTransportListener::TcpTransportListener()
    : _listenHandle(g_loop->resource<uvw::tcp_handle>()) {}

void TcpTransportListener::SetConnectionFactory(ConnectionFactory factory) {
  _factory = std::move(factory);
}

bool TcpTransportListener::Listen(const std::string& host, int port) {
  _listenHandle->on<uvw::listen_event>(
      [this](const uvw::listen_event&, uvw::tcp_handle& srv) {
        if (!_factory) {
          return;
        }
        std::shared_ptr<uvw::tcp_handle> client =
            srv.parent().resource<uvw::tcp_handle>();
        srv.accept(*client);
        _factory(std::make_unique<TcpTransportConnection>(std::move(client)));
      });

  _listenHandle->bind(host, port);
  _listenHandle->listen();
  return true;
}

}  // namespace Ardos
