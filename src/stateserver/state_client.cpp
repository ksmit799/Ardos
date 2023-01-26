#include "state_client.h"

#include "../util/logger.h"

namespace Ardos {

StateClient::StateClient(const std::shared_ptr<uvw::TCPHandle> &socket) {
  _socket = socket;

  // Disable Nagle's algorithm and ensure the connection is live.
  _socket->noDelay(true);
  _socket->keepAlive(true, uvw::TCPHandle::Time{60});

  _remoteAddress = _socket->peer();

  // Setup event listeners.
  _socket->on<uvw::ErrorEvent>(
      [this](const uvw::ErrorEvent &event, uvw::TCPHandle &) {
        this->HandleDisconnect((uv_errno_t)event.code());
      });

  _socket->on<uvw::EndEvent>([this](const uvw::EndEvent &, uvw::TCPHandle &) {
    this->HandleDisconnect(UV_EOF);
  });

  _socket->on<uvw::CloseEvent>(
      [this](const uvw::CloseEvent &, uvw::TCPHandle &) {
        this->HandleDisconnect(UV_EOF);
      });

  Logger::Info(std::format("State Server client connected from {}:{}",
                           _remoteAddress.ip, _remoteAddress.port));

  _socket->read();
}

StateClient::~StateClient() {

}

void StateClient::HandleDisconnect(uv_errno_t errorCode) {}

} // namespace Ardos