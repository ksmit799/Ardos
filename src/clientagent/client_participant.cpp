#include "client_participant.h"

#include "../util/logger.h"

namespace Ardos {

ClientParticipant::ClientParticipant(
    ClientAgent *clientAgent, const std::shared_ptr<uvw::TCPHandle> &socket)
    : NetworkClient(socket), ChannelSubscriber(), _clientAgent(clientAgent) {
  auto address = GetRemoteAddress();
  Logger::Verbose(std::format("[CA] Client connected from {}:{}", address.ip,
                              address.port));
}

/**
 * Manually disconnect and delete this client participant.
 */
void ClientParticipant::Shutdown() {
  ChannelSubscriber::Shutdown();

  delete this;
}

/**
 * Handles socket disconnect events.
 * @param code
 */
void ClientParticipant::HandleDisconnect(uv_errno_t code) {
  auto address = GetRemoteAddress();

  auto errorEvent = uvw::ErrorEvent{(int)code};
  Logger::Verbose(std::format("[CA] Lost connection from {}:{}: {}", address.ip,
                              address.port, errorEvent.what()));

  Shutdown();
}

void ClientParticipant::HandleClientDatagram(
    const std::shared_ptr<Datagram> &dg) {}

void ClientParticipant::HandleDatagram(const std::shared_ptr<Datagram> &dg) {}

} // namespace Ardos