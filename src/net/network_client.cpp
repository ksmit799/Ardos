#include "network_client.h"

namespace Ardos {

NetworkClient::NetworkClient(const std::shared_ptr<uvw::TCPHandle> &socket)
    : _socket(socket) {
  // Configure socket options.
  _socket->noDelay(true);
  _socket->keepAlive(true, uvw::TCPHandle::Time{60});

  _remoteAddress = _socket->peer();

  // Setup event listeners.
  _socket->on<uvw::ErrorEvent>(
      [this](const uvw::ErrorEvent &event, uvw::TCPHandle &) {
        HandleClose((uv_errno_t)event.code());
      });

  _socket->on<uvw::EndEvent>([this](const uvw::EndEvent &, uvw::TCPHandle &) {
    HandleClose(UV_EOF);
  });

  _socket->on<uvw::CloseEvent>(
      [this](const uvw::CloseEvent &, uvw::TCPHandle &) {
        HandleClose(UV_EOF);
      });

  _socket->on<uvw::DataEvent>(
      [this](const uvw::DataEvent &event, uvw::TCPHandle &) {
        HandleData(event.data, event.length);
      });

  _socket->read();
}

NetworkClient::~NetworkClient() {
  Shutdown();
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
uvw::Addr NetworkClient::GetRemoteAddress() { return _remoteAddress; }

void NetworkClient::Shutdown() {
  _disconnected = true;

  if (_socket) {
    _socket->close();
    _socket.reset();
  }
}

void NetworkClient::HandleClose(uv_errno_t code) {
  if (_disconnected) {
    // We've already handled this clients disconnect.
    return;
  }

  _disconnected = true;

  HandleDisconnect(code);
}

void NetworkClient::HandleData(const std::unique_ptr<char[]> &data,
                               size_t size) {
  // We can't directly handle datagrams as it's possible that multiple have been
  // buffered together, or we've received a split message.

  // First, check if we have one, complete datagram.
  if (_data_buf.empty() && size >= sizeof(uint16_t)) {
    // Ok, we at least have a size header. Let's check if we have the full
    // datagram.
    uint16_t datagramSize = *reinterpret_cast<uint16_t *>(data.get());
    if (datagramSize == size - sizeof(uint16_t)) {
      // We have a complete datagram, lets handle it.
      auto dg = std::make_shared<Datagram>(
          reinterpret_cast<const uint8_t *>(data.get() + sizeof(uint16_t)),
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
    uint16_t dataSize = *reinterpret_cast<uint16_t *>(&_data_buf[0]);
    if (_data_buf.size() >= dataSize + sizeof(uint16_t)) {
      // We have a complete datagram!
      auto dg = std::make_shared<Datagram>(
          reinterpret_cast<const uint8_t *>(&_data_buf[sizeof(uint16_t)]),
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
 * Sends a datagram to this network client.
 * @param dg
 */
void NetworkClient::SendDatagram(const std::shared_ptr<Datagram> &dg) {
  size_t sendSize = sizeof(uint16_t) + dg->Size();
  auto sendBuffer = std::unique_ptr<char[]>(new char[sendSize]);

  uint16_t dgSize = dg->Size();

  auto sendPtr = sendBuffer.get();
  // Datagram size tag.
  memcpy(sendPtr, (char *)&dgSize, sizeof(uint16_t));
  // Datagram data.
  memcpy(sendPtr + sizeof(uint16_t), dg->GetData(), dg->Size());

  _socket->write(std::move(sendBuffer), sendSize);
}

} // namespace Ardos
