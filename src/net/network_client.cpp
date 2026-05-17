#include "network_client.h"

#include <spdlog/spdlog.h>

#include <cstring>
#include <limits>

namespace Ardos {

NetworkClient::NetworkClient(const std::shared_ptr<uvw::tcp_handle>& socket)
    : _socket(socket) {
  // Configure socket options.
  _socket->no_delay(true);
  _socket->keep_alive(true, uvw::tcp_handle::time{60});

  _remoteAddress = _socket->peer();
  _localAddress = _socket->sock();

  // Setup event listeners. Each lambda captures `_alive` so a late
  // event firing on a destroyed `this` no-ops instead of derefing
  // freed memory (uvw close() is async).
  _socket->on<uvw::error_event>(
      [this, alive = _alive](const uvw::error_event& event, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        HandleClose((uv_errno_t)event.code());
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
        if (!*alive || _disconnected) {
          return;
        }
        HandleData(event.data, event.length);
      });

  _socket->on<uvw::write_event>(
      [this, alive = _alive](const uvw::write_event& event, uvw::tcp_handle&) {
        if (!*alive) {
          return;
        }
        _isWriting = false;
        if (_disconnected) {
          // Drop anything still queued — the peer is going away. Close once
          // the in-flight uv_write has resolved (this callback).
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

NetworkClient::~NetworkClient() {
  NetworkClient::Shutdown();
  // Force-close even if a write was in flight; we're being destroyed,
  // graceful flush is no longer relevant. The pending write callback
  // will still fire asynchronously but *_alive = false makes it no-op.
  if (!_socketClosed) {
    _socket->close();
    _socketClosed = true;
  }
  // Flip alive last so any late-firing uvw event after the dtor no-ops.
  *_alive = false;
}

/**
 * Returns whether or not this client is in a disconnected state.
 * @return
 */
bool NetworkClient::Disconnected() const { return _disconnected; }

/**
 * Returns this clients remote address.
 * @return
 */
uvw::socket_address NetworkClient::GetRemoteAddress() const {
  return _remoteAddress;
}

/**
 * Returns this clients local address.
 * @return
 */
uvw::socket_address NetworkClient::GetLocalAddress() const {
  return _localAddress;
}

void NetworkClient::Shutdown() {
  if (_disconnected) {
    return;
  }

  _disconnected = true;

  // Abandon anything queued; we're not going to flush it.
  _writeQueue.clear();
  _queuedBytes = 0;

  // Defer close to the write_event callback if a uv_write is in flight —
  // *_alive stays true so the callback can still see _disconnected and
  // run the close path. (The dtor force-closes if we're still pending.)
  if (!_isWriting && !_socketClosed) {
    _socket->close();
    _socketClosed = true;
  }
}

void NetworkClient::HandleClose(uv_errno_t code) {
  if (_disconnected) {
    // We've already handled this clients disconnect.
    return;
  }

  _socketClosed = true;

  HandleDisconnect(code);
}

// NOLINTNEXTLINE(modernize-avoid-c-arrays): unique_ptr<char[]> from uvw read
void NetworkClient::HandleData(const std::unique_ptr<char[]>& data,
                               size_t size) {
  // We can't directly handle datagrams as it's possible that multiple have been
  // buffered together, or we've received a split message.

  // First, check if we have one, complete datagram.
  if (_data_buf.empty() && size >= sizeof(uint16_t)) {
    // Ok, we at least have a size header. Let's check if we have the full
    // datagram.
    uint16_t datagramSize;
    std::memcpy(&datagramSize, data.get(), sizeof(datagramSize));
    if (datagramSize == size - sizeof(uint16_t)) {
      // We have a complete datagram, lets handle it.
      auto dg = std::make_shared<Datagram>(
          reinterpret_cast<const uint8_t*>(data.get() + sizeof(uint16_t)),
          datagramSize);
      HandleClientDatagram(dg);
      return;
    }
  }

  // Hmm, we don't. Let's put it into our buffer.
  _data_buf.insert(_data_buf.end(), data.get(), data.get() + size);
  ProcessBuffer();
}

void NetworkClient::ProcessBuffer() {
  while (_data_buf.size() > sizeof(uint16_t)) {
    // We have enough data to know the expected length of the datagram.
    uint16_t dataSize;
    std::memcpy(&dataSize, _data_buf.data(), sizeof(dataSize));
    if (_data_buf.size() >= dataSize + sizeof(uint16_t)) {
      // We have a complete datagram!
      auto dg = std::make_shared<Datagram>(
          reinterpret_cast<const uint8_t*>(&_data_buf[sizeof(uint16_t)]),
          dataSize);

      // Remove the datagram data from the buffer.
      _data_buf.erase(_data_buf.begin(),
                      _data_buf.begin() + sizeof(uint16_t) + dataSize);

      HandleClientDatagram(dg);
    } else {
      return;
    }
  }
}

/**
 * Sends a datagram to this network client. Buffers go onto an application-
 * level FIFO drained one uv_write at a time; a peer that lets the queue
 * exceed kHighWaterBytes is force-disconnected.
 */
void NetworkClient::SendDatagram(const std::shared_ptr<Datagram>& dg) {
  if (_socket == nullptr || _disconnected) {
    return;
  }

  // Framing prefix is uint16; larger payloads would wrap and desync the peer.
  if (dg->Size() > std::numeric_limits<uint16_t>::max()) {
    spdlog::get("md")->error(
        "Network client refusing oversized datagram ({}B > {}B max)",
        dg->Size(), std::numeric_limits<uint16_t>::max());
    return;
  }

  const size_t sendSize = sizeof(uint16_t) + dg->Size();

  if (_queuedBytes + sendSize > kHighWaterBytes) {
    spdlog::get("md")->warn(
        "Network client {}:{} exceeded {}B write backlog; disconnecting",
        _remoteAddress.ip, _remoteAddress.port, kHighWaterBytes);
    Shutdown();
    return;
  }

  // runtime-sized buffer for uvw write:
  // NOLINTNEXTLINE(modernize-avoid-c-arrays)
  auto sendBuffer = std::unique_ptr<char[]>(new char[sendSize]);
  const uint16_t dgSize = dg->Size();
  auto* const sendPtr = &sendBuffer.get()[0];
  memcpy(sendPtr, &dgSize, sizeof(uint16_t));
  memcpy(sendPtr + sizeof(uint16_t), dg->GetData(), dg->Size());

  _writeQueue.push_back({std::move(sendBuffer), sendSize});
  _queuedBytes += sendSize;

  PumpWrite();
}

void NetworkClient::PumpWrite() {
  if (_isWriting || _writeQueue.empty() || _socketClosed) {
    return;
  }
  auto front = std::move(_writeQueue.front());
  _writeQueue.pop_front();
  _queuedBytes -= front.size;
  _isWriting = true;
  _socket->write(std::move(front.buf), front.size);
}

}  // namespace Ardos
