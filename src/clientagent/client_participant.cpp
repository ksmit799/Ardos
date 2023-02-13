#include "client_participant.h"

#include "../net/message_types.h"
#include "../util/logger.h"

namespace Ardos {

ClientParticipant::ClientParticipant(
    ClientAgent *clientAgent, const std::shared_ptr<uvw::TCPHandle> &socket)
    : NetworkClient(socket), ChannelSubscriber(), _clientAgent(clientAgent) {
  auto address = GetRemoteAddress();
  Logger::Verbose(std::format("[CA] Client connected from {}:{}", address.ip,
                              address.port));

  _channel = _clientAgent->AllocateChannel();
  if (!_channel) {
    Logger::Error("[CA] Channel range depleted!");
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "Channel range depleted");
    return;
  }

  SubscribeChannel(_channel);
  SubscribeChannel(BCHAN_CLIENTS);
}

/**
 * Manually disconnect and delete this client participant.
 */
void ClientParticipant::Shutdown() {
  ChannelSubscriber::Shutdown();
  NetworkClient::Shutdown();

  delete this;
}

void ClientParticipant::Annihilate() {
  // Unsubscribe from all channels so DELETE messages aren't sent back to us.
  ChannelSubscriber::Shutdown();
  _clientAgent->FreeChannel(_channel);

  // Delete all session objects.

  Shutdown();
}

/**
 * Handles socket disconnect events.
 * @param code
 */
void ClientParticipant::HandleDisconnect(uv_errno_t code) {
  if (!_cleanDisconnect) {
    auto address = GetRemoteAddress();

    auto errorEvent = uvw::ErrorEvent{(int)code};
    Logger::Verbose(std::format("[CA] Lost connection from {}:{}: {}",
                                address.ip, address.port, errorEvent.what()));
  }

  Annihilate();
}

/**
 * Handles a datagram incoming from the client.
 * @param dg
 */
void ClientParticipant::HandleClientDatagram(
    const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);
  try {
    switch (_authState) {
    case AUTH_STATE_NEW:
      HandlePreHello(dgi);
      break;
    case AUTH_STATE_ANONYMOUS:
      HandlePreAuth(dgi);
      break;
    case AUTH_STATE_ESTABLISHED:
      HandleAuthenticated(dgi);
      break;
    }
  } catch (const DatagramIteratorEOF &) {
    SendDisconnect(CLIENT_DISCONNECT_TRUNCATED_DATAGRAM,
                   "Datagram unexpectedly ended while iterating.");
    return;
  } catch (const DatagramOverflow &) {
    SendDisconnect(CLIENT_DISCONNECT_OVERSIZED_DATAGRAM,
                   "Internal datagram too large to be routed.", true);
    return;
  }

  // We shouldn't have any remaining data left after handling it.
  // If we do, assume the client has *somehow* appended junk data on the end and
  // disconnect them to be safe.
  if (dgi.GetRemainingSize()) {
    SendDisconnect(CLIENT_DISCONNECT_OVERSIZED_DATAGRAM,
                   "Datagram contains excess data.", true);
    return;
  }
}

/**
 * Handles a datagram incoming from the Message Director.
 * @param dg
 */
void ClientParticipant::HandleDatagram(const std::shared_ptr<Datagram> &dg) {}

/**
 * Disconnects this client with a reason and message.
 * @param reason
 * @param message
 * @param security
 */
void ClientParticipant::SendDisconnect(const uint16_t &reason,
                                       const std::string &message,
                                       const bool &security) {
  if (Disconnected()) {
    return;
  }

  std::string logOut = std::format("[CA] Ejecting client: '{}': {} - {}",
                                   _channel, reason, message);
  security ? Logger::Error(logOut) : Logger::Warn(logOut);

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_EJECT);
  dg->AddUint16(reason);
  dg->AddString(message);
  SendDatagram(dg);

  // This will call Annihilate from HandleDisconnect.
  _cleanDisconnect = true;
  NetworkClient::Shutdown();
}

void ClientParticipant::HandlePreHello(DatagramIterator &dgi) {}

void ClientParticipant::HandlePreAuth(DatagramIterator &dgi) {}

void ClientParticipant::HandleAuthenticated(DatagramIterator &dgi) {}

} // namespace Ardos