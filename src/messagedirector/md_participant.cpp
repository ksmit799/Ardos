#include "md_participant.h"

#include "../util/logger.h"

namespace Ardos {

MDParticipant::MDParticipant(const std::shared_ptr<uvw::TCPHandle> &socket)
    : _socket(socket) {
  // Configure socket options.
  _socket->noDelay(true);
  _socket->keepAlive(true, uvw::TCPHandle::Time{60});

  _remoteAddress = _socket->peer();

  // Setup event listeners.
  _socket->on<uvw::ErrorEvent>(
      [this](const uvw::ErrorEvent &event, uvw::TCPHandle &) {
        HandleDisconnect((uv_errno_t)event.code());
      });

  _socket->on<uvw::EndEvent>([this](const uvw::EndEvent &, uvw::TCPHandle &) {
    HandleDisconnect(UV_EOF);
  });

  _socket->on<uvw::CloseEvent>(
      [this](const uvw::CloseEvent &, uvw::TCPHandle &) {
        HandleDisconnect(UV_EOF);
      });

  _socket->on<uvw::DataEvent>([this](const uvw::DataEvent &event,
                                     uvw::TCPHandle &) {
    auto dg = std::make_shared<Datagram>(
        reinterpret_cast<const uint8_t *>(event.data.get() + sizeof(uint16_t)),
        event.length);
    HandleDatagram(dg);
  });

  Logger::Info(std::format("[MD] Participant connected from {}:{}",
                           _remoteAddress.ip, _remoteAddress.port));

  _socket->read();
}

MDParticipant::~MDParticipant() {
  _socket->close();
  _socket.reset();
}

/**
 * Manually disconnect and delete this MD participant.
 */
void MDParticipant::Shutdown() {
  _disconnected = true;
  delete this;
}

/**
 * Handles socket disconnect events.
 * @param code
 */
void MDParticipant::HandleDisconnect(uv_errno_t code) {
  if (_disconnected) {
    // We've already handled this participants disconnect.
    return;
  }

  _disconnected = true;

  auto errorEvent = uvw::ErrorEvent{(int)code};
  Logger::Info(std::format("[MD] Lost connection from '{}' ({}:{}): {}",
                           _connName, _remoteAddress.ip, _remoteAddress.port,
                           errorEvent.what()));

  Shutdown();
}

void MDParticipant::HandleDatagram(const std::shared_ptr<Datagram> &dg) {}

} // namespace Ardos
