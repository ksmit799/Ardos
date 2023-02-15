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

void ClientParticipant::HandlePreHello(DatagramIterator &dgi) {
  uint16_t msgType = dgi.GetUint16();

#ifdef ARDOS_USE_LEGACY_CLIENT
  switch (msgType) {
  case CLIENT_LOGIN_FAIRIES:
  case CLIENT_LOGIN_TOONTOWN:
    HandleLoginLegacy(dgi);
    break;
  default:
    SendDisconnect(CLIENT_DISCONNECT_NO_HELLO, "First packet is not LOGIN");
  }
#else
  if (msgType != CLIENT_HELLO) {
    SendDisconnect(CLIENT_DISCONNECT_NO_HELLO,
                   "First packet is not CLIENT_HELLO");
    return;
  }

  uint32_t hashVal = dgi.GetUint32();
  std::string version = dgi.GetString();

  if (version != _clientAgent->GetVersion()) {
    SendDisconnect(CLIENT_DISCONNECT_BAD_VERSION,
                   "Your client is out-of-date!");
    return;
  }

  if (hashVal != _clientAgent->GetHash()) {
    SendDisconnect(CLIENT_DISCONNECT_BAD_DCHASH, "Mismatched DC hash!", true);
    return;
  }

  _authState = AUTH_STATE_ANONYMOUS;

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_HELLO_RESP);
  SendDatagram(dg);
#endif // ARDOS_USE_LEGACY_CLIENT
}

#ifdef ARDOS_USE_LEGACY_CLIENT
void ClientParticipant::HandleLoginLegacy(DatagramIterator &dgi) {
  uint64_t authShim = _clientAgent->GetAuthShim();
  if (!authShim) {
    Logger::Error("[CA] No configured auth shim for legacy login!");
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "No available login handler!");
    return;
  }

  std::string loginToken = dgi.GetString();
  std::string clientVersion = dgi.GetString();
  uint32_t hashVal = dgi.GetUint32();
  dgi.GetUint32(); // Token type.
  dgi.GetString(); // Unused.

  if (clientVersion != _clientAgent->GetVersion()) {
    SendDisconnect(CLIENT_DISCONNECT_BAD_VERSION,
                   "Your client is out-of-date!");
    return;
  }

  if (hashVal != _clientAgent->GetHash()) {
    SendDisconnect(CLIENT_DISCONNECT_BAD_DCHASH, "Mismatched DC hash!", true);
    return;
  }

  // We've got a matching version and hash, send off the login request to the
  // configured shim UberDOG!
  auto dg = std::make_shared<Datagram>(authShim, _channel,
                                       STATESERVER_OBJECT_SET_FIELD);
  dg->AddUint32(authShim);
  dg->AddUint16(0); // TODO: Field number.
  dg->AddString(loginToken);
  PublishDatagram(dg);
}
#endif // ARDOS_USE_LEGACY_CLIENT

void ClientParticipant::HandlePreAuth(DatagramIterator &dgi) {
  uint16_t msgType = dgi.GetUint16();
  switch (msgType) {
  case CLIENT_DISCONNECT: {
    _cleanDisconnect = true;
    NetworkClient::Shutdown();
    break;
  }
  case CLIENT_OBJECT_SET_FIELD:
    HandleClientObjectUpdateField(dgi);
    break;
  case CLIENT_HEARTBEAT:
    HandleClientHeartbeat();
    break;
  default:
    SendDisconnect(CLIENT_DISCONNECT_ANONYMOUS_VIOLATION, "", true);
  }
}

void ClientParticipant::HandleAuthenticated(DatagramIterator &dgi) {}

} // namespace Ardos