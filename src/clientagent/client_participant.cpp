#include "client_participant.h"

#include <dcField.h>

#include "../net/message_types.h"
#include "../util/globals.h"
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

  if (_clientAgent->GetHeartbeatInterval()) {
    // Set up the heartbeat timeout timer.
    _heartbeatTimer = g_loop->resource<uvw::TimerHandle>();
    _heartbeatTimer->on<uvw::TimerEvent>(
        [this](const uvw::TimerEvent &, uvw::TimerHandle &) {
          HandleHeartbeatTimeout();
        });
  }

  if (_clientAgent->GetAuthTimeout()) {
    // Set up the auth timeout timer.
    _authTimer = g_loop->resource<uvw::TimerHandle>();
    _authTimer->on<uvw::TimerEvent>(
        [this](const uvw::TimerEvent &, uvw::TimerHandle &) {
          HandleAuthTimeout();
        });

    _authTimer->start(uvw::TimerHandle::Time{_clientAgent->GetAuthTimeout()},
                      uvw::TimerHandle::Time{0});
  }
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
  // Stop the heartbeat timer (if we have one.)
  if (_heartbeatTimer) {
    _heartbeatTimer->stop();
    _heartbeatTimer->close();
    _heartbeatTimer.reset();
  }

  // Stop the auth timer (if we have one.)
  if (_authTimer) {
    _authTimer->stop();
    _authTimer->close();
    _authTimer.reset();
  }

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

/**
 * Resets this client's heartbeat disconnect timer.
 */
void ClientParticipant::HandleClientHeartbeat() {
  if (_heartbeatTimer) {
    _heartbeatTimer->stop();
    _heartbeatTimer->start(
        uvw::TimerHandle::Time{_clientAgent->GetHeartbeatInterval()},
        uvw::TimerHandle::Time{0});
  }
}

/**
 * This client hasn't sent a heartbeat packet in the required interval.
 * Disconnect them.
 */
void ClientParticipant::HandleHeartbeatTimeout() {
  // Stop the heartbeat timer.
  _heartbeatTimer->stop();
  _heartbeatTimer->close();
  _heartbeatTimer.reset();

  SendDisconnect(CLIENT_DISCONNECT_NO_HEARTBEAT,
                 "Client did not send heartbeat in required interval");
}

/**
 * Check to see if this client has authenticated within the required time.
 */
void ClientParticipant::HandleAuthTimeout() {
  // Stop the auth timer.
  _authTimer->stop();
  _authTimer->close();
  _authTimer.reset();

  if (_authState != AUTH_STATE_ESTABLISHED) {
    SendDisconnect(CLIENT_DISCONNECT_GENERIC,
                   "Client did not authenticate in the required time");
  }
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

  HandleClientHeartbeat();

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
  uint32_t authShim = _clientAgent->GetAuthShim();
  if (!authShim) {
    Logger::Error("[CA] No configured auth shim for legacy login!");
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "No available login handler!");
    return;
  }

  HandleClientHeartbeat();

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
    SendDisconnect(
        CLIENT_DISCONNECT_ANONYMOUS_VIOLATION,
        std::format("Message: {} not allowed prior to authentication!",
                    msgType),
        true);
  }
}

void ClientParticipant::HandleAuthenticated(DatagramIterator &dgi) {
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
  case CLIENT_OBJECT_LOCATION:
    HandleClientObjectLocation(dgi);
    break;
  case CLIENT_ADD_INTEREST:
    HandleClientAddInterest(dgi, false);
    break;
  case CLIENT_ADD_INTEREST_MULTIPLE:
    HandleClientAddInterest(dgi, true);
    break;
  case CLIENT_REMOVE_INTEREST:
    HandleClientRemoveInterest(dgi);
    break;
  case CLIENT_HEARTBEAT:
    HandleClientHeartbeat();
    break;
  default:
    SendDisconnect(CLIENT_DISCONNECT_INVALID_MSGTYPE,
                   std::format("Client sent invalid message: {}", msgType),
                   true);
  }
}

/**
 * Lookup a Distributed Object in-view of this client and return its class.
 * @param doId
 */
DCClass *ClientParticipant::LookupObject(const uint32_t &doId) {
  // First, see if it's an UberDOG.
  auto uberdogs = _clientAgent->Uberdogs();
  if (uberdogs.contains(doId)) {
    return uberdogs[doId].dcc;
  }

  // Next, check the object cache, but this client only knows about it
  // if it occurs in seen objects or owned objects.
  if (_ownedObjects.contains(doId)) {
    return _ownedObjects[doId].dcc;
  }
  if (_seenObjects.contains(doId) && _visibleObjects.contains(doId)) {
    return _visibleObjects[doId].dcc;
  }
  if (_declaredObjects.contains(doId)) {
    return _declaredObjects[doId].dcc;
  }

  return nullptr;
}

/**
 * The client is attempting to update a field on a Distributed Object.
 * @param dgi
 */
void ClientParticipant::HandleClientObjectUpdateField(DatagramIterator &dgi) {
  uint32_t doId = dgi.GetUint32();
  uint16_t fieldId = dgi.GetUint16();

  DCClass *dcc = LookupObject(doId);
  if (!dcc) {
    if (_historicalObjects.contains(doId)) {
      // The client isn't disconnected in this case because it could be a
      // delayed message for a once visible object. Make sure to skip the rest
      // of the payload to simulate the message being handled correctly.
      dgi.Skip(dgi.GetRemainingSize());
    } else {
      SendDisconnect(
          CLIENT_DISCONNECT_MISSING_OBJECT,
          std::format("Client tried to send update to non-existent object: {}",
                      doId));
    }
    return;
  }

  // If the client is not in an established auth state, it may only send updates
  // to anonymous UberDOG's.
  auto uberdogs = _clientAgent->Uberdogs();
  if (_authState != AUTH_STATE_ESTABLISHED) {
    if (!uberdogs.contains(doId) || !uberdogs[doId].anonymous) {
      SendDisconnect(
          CLIENT_DISCONNECT_ANONYMOUS_VIOLATION,
          std::format("Client tried to send update to non-anonymous object: {}",
                      doId),
          true);
      return;
    }
  }

  // Check that the field id actually exists on the object.
  DCField *field = dcc->get_field_by_index(fieldId);
  if (!field) {
    SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_FIELD,
                   std::format("Client tried to send update to non-existent "
                               "field: {} on object: {}",
                               fieldId, doId),
                   true);
    return;
  }

  // Check that the client is actually allowed to send updates to this field.
  bool isOwned = _ownedObjects.contains(doId);
  if (!field->is_clsend() && !(isOwned && field->is_ownsend())) {
    if (!_fieldsSendable.contains(doId) ||
        !_fieldsSendable[doId].contains(fieldId)) {
      SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_FIELD,
                     std::format("Client tried to send update to non-sendable "
                                 "field: {} of class: {} (DoId: {})",
                                 field->get_name(), dcc->get_name(), doId));
      return;
    }
  }

  std::vector<uint8_t> data;
  dgi.UnpackField(field, data);

  // Forward the field update to the state server.
  auto dg =
      std::make_shared<Datagram>(doId, _channel, STATESERVER_OBJECT_SET_FIELD);
  dg->AddUint32(doId);
  dg->AddUint16(fieldId);
  dg->AddData(data);
  PublishDatagram(dg);
}

void ClientParticipant::HandleClientObjectLocation(DatagramIterator &dgi) {
  // Make sure client owned objects are relocatable (clients can update their
  // location.)
  if (!_clientAgent->GetRelocateAllowed()) {
    SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_RELOCATE,
                   "Client object relocation is disallowed", true);
    return;
  }

  uint32_t doId = dgi.GetUint32();

  // Make sure the object the client is trying to update actually exists and
  // that they're allowed to update it.
  bool isOwned = _ownedObjects.contains(doId);
  if (!isOwned) {
    if (_historicalObjects.contains(doId)) {
      // The client isn't disconnected in this case because it could be a
      // delayed message for a once visible object. Make sure to skip the rest
      // of the payload to simulate the message being handled correctly.
      dgi.Skip(dgi.GetRemainingSize());
    } else if (_visibleObjects.contains(doId)) {
      // They can't relocate an object they don't own.
      SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_RELOCATE,
                     "Client attempted to relocate object they don't own",
                     true);
    } else {
      SendDisconnect(
          CLIENT_DISCONNECT_MISSING_OBJECT,
          std::format("Client tried to relocate unknown object: {}", doId),
          true);
    }
    return;
  }

  // Update the object's location with the state server.
  auto dg = std::make_shared<Datagram>(doId, _channel,
                                       STATESERVER_OBJECT_SET_LOCATION);
  dg->AddLocation(dgi.GetUint32(), dgi.GetUint32());
  PublishDatagram(dg);
}

void ClientParticipant::HandleClientAddInterest(DatagramIterator &dgi,
                                                const bool &multiple) {
  if (_clientAgent->GetInterestsPermission() == INTERESTS_DISABLED) {
    SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
                   "Client is not allowed to add interests", true);
    return;
  }

  uint32_t context = dgi.GetUint32();

  Interest i;
  BuildInterest(dgi, multiple, i);
  if (_clientAgent->GetInterestsPermission() == INTERESTS_VISIBLE &&
      !LookupObject(i.parent)) {
    SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
                   std::format("Client cannot add interest to parent with id: "
                               "{} as parent is not visible",
                               i.parent),
                   true);
    return;
  }
  AddInterest(i, context);
}

void ClientParticipant::HandleClientRemoveInterest(DatagramIterator &dgi) {}

void ClientParticipant::BuildInterest(DatagramIterator &dgi,
                                      const bool &multiple, Interest &out) {
  uint16_t interestId = dgi.GetUint16();
  uint32_t parent = dgi.GetUint32();

  out.id = interestId;
  out.parent = parent;

  uint16_t count = 1;
  if (multiple) {
    count = dgi.GetUint16();
  }

  for (uint16_t i = 0; i < count; ++i) {
    out.zones.insert(dgi.GetUint32());
  }
}

void ClientParticipant::AddInterest(Interest &i, const uint32_t &context,
                                    const uint64_t &caller) {
  std::unordered_set<uint32_t> newZones;
  for (const auto &zone : i.zones) {
    if (LookupInterests(i.parent, zone).empty()) {
      newZones.insert(zone);
    }
  }

  // This is an already open interest handle that the client is modifying.
  if (_interests.contains(i.id)) {
    Interest previousInterest = _interests[i.id];

    std::unordered_set<uint32_t> killedZones;
    for (const auto &zone : previousInterest.zones) {
      if (!LookupInterests(previousInterest.parent, zone).empty()) {
        // An interest other than the altered one can see this parent/zone, so
        // we don't care about it.
        continue;
      }

      if (i.parent != previousInterest.parent || !i.zones.contains(zone)) {
        killedZones.insert(zone);
      }
    }

    CloseZones(previousInterest.parent, killedZones);
  }

  _interests[i.id] = i;

  if (newZones.empty()) {
    // We aren't requesting any new zones with operation, so no need to poke the
    // state server.
    NotifyInterestDone(i.id, caller);
    HandleInterestDone(i.id, context);
    return;
  }

  uint32_t requestContext = _nextContext++;

  // Create and store a new interest operation for the incoming objects from the
  // state server.
  auto iop = new InterestOperation(this, _clientAgent->GetInterestTimeout(),
                                   i.id, context, requestContext, i.parent,
                                   newZones, caller);
  _pendingInterests[requestContext] = iop;

  auto dg = std::make_shared<Datagram>(i.parent, _channel,
                                       STATESERVER_OBJECT_GET_ZONES_OBJECTS);
  dg->AddUint32(requestContext);
  dg->AddUint32(i.parent);
  dg->AddUint16(newZones.size());
  for (const auto &zone : newZones) {
    dg->AddUint32(zone);
    SubscribeChannel(LocationAsChannel(i.parent, zone));
  }
  PublishDatagram(dg);
}

std::vector<Interest>
ClientParticipant::LookupInterests(const uint32_t &parentId,
                                   const uint32_t &zoneId) {
  std::vector<Interest> interests;
  for (const auto &interest : _interests) {
    if (parentId == interest.second.parent &&
        interest.second.zones.contains(zoneId)) {
      interests.push_back(interest.second);
    }
  }

  return interests;
}

void ClientParticipant::NotifyInterestDone(const uint16_t &interestId,
                                           const uint64_t &caller) {
  if (!caller) {
    return;
  }

  auto dg = std::make_shared<Datagram>(caller, _channel,
                                       CLIENTAGENT_DONE_INTEREST_RESP);
  dg->AddUint64(_channel);
  dg->AddUint16(interestId);
  PublishDatagram(dg);
}

void ClientParticipant::HandleInterestDone(const uint16_t &interestId,
                                           const uint32_t &context) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_DONE_INTEREST_RESP);
  dg->AddUint32(context);
  dg->AddUint16(interestId);
  SendDatagram(dg);
}

void ClientParticipant::CloseZones(
    const uint32_t &parent, const std::unordered_set<uint32_t> &killedZones) {
  std::vector<uint32_t> toRemove;
  for (const auto &objects : _visibleObjects) {
    const VisibleObject &visibleObject = objects.second;
    if (visibleObject.parent != parent) {
      // Object does not belong to parent, ignore.
      continue;
    }

    if (killedZones.contains(visibleObject.zone)) {
      if (_sessionObjects.contains(visibleObject.doId)) {
        // This object is a session object, if it's being closed, so should the
        // client.
        SendDisconnect(CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
                       "A session object has unexpectedly left interest");
        return;
      }

      HandleRemoveObject(visibleObject.doId);

      _seenObjects.erase(visibleObject.doId);
      _historicalObjects.insert(visibleObject.doId);
      toRemove.push_back(visibleObject.doId);
    }
  }

  for (const auto &doId : toRemove) {
    _visibleObjects.erase(doId);
  }

  // Close out killed channels.
  for (const auto &zone : killedZones) {
    UnsubscribeChannel(LocationAsChannel(parent, zone));
  }
}

void ClientParticipant::HandleRemoveObject(const uint32_t &doId) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_LEAVING);
  dg->AddUint32(doId);
  SendDatagram(dg);
}

} // namespace Ardos
