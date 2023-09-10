#include "client_participant.h"

#include <dcField.h>

#include "../net/message_types.h"
#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

ClientParticipant::ClientParticipant(
    ClientAgent *clientAgent, const std::shared_ptr<uvw::tcp_handle> &socket)
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

  _allocatedChannel = _channel;

  SubscribeChannel(_channel);
  SubscribeChannel(BCHAN_CLIENTS);

  if (_clientAgent->GetHeartbeatInterval()) {
    // Set up the heartbeat timeout timer.
    _heartbeatTimer = g_loop->resource<uvw::timer_handle>();
    _heartbeatTimer->on<uvw::timer_event>(
        [this](const uvw::timer_event &, uvw::timer_handle &) {
          HandleHeartbeatTimeout();
        });
  }

  if (_clientAgent->GetAuthTimeout()) {
    // Set up the auth timeout timer.
    _authTimer = g_loop->resource<uvw::timer_handle>();
    _authTimer->on<uvw::timer_event>(
        [this](const uvw::timer_event &, uvw::timer_handle &) {
          HandleAuthTimeout();
        });

    _authTimer->start(uvw::timer_handle::time{_clientAgent->GetAuthTimeout()},
                      uvw::timer_handle::time{0});
  }

  _clientAgent->ParticipantJoined();
}

ClientParticipant::~ClientParticipant() {
  NetworkClient::Shutdown();

  _clientAgent->ParticipantLeft();
}

/**
 * Manually disconnect and delete this client participant.
 */
void ClientParticipant::Shutdown() {
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
  _clientAgent->FreeChannel(_allocatedChannel);

  // Delete all session objects.
  while (!_sessionObjects.empty()) {
    uint32_t doId = *_sessionObjects.begin();
    _sessionObjects.erase(doId);
    Logger::Verbose(std::format(
        "[CA] Client: {} exited, deleting session object: {}", _channel, doId));

    auto dg = std::make_shared<Datagram>(doId, _channel,
                                         STATESERVER_OBJECT_DELETE_RAM);
    dg->AddUint32(doId);
    PublishDatagram(dg);
  }

  // Clear out all pending interest operations.
  // Note: PendingInterests delete themselves on Finish, so we have to be
  // careful how we advance 'it'.
  for (auto it = _pendingInterests.begin(); it != _pendingInterests.end();) {
    (it++)->second->Finish();
  }
}

/**
 * Handles socket disconnect events.
 * @param code
 */
void ClientParticipant::HandleDisconnect(uv_errno_t code) {
  if (!_cleanDisconnect) {
    auto address = GetRemoteAddress();

    auto errorEvent = uvw::error_event{(int)code};
    Logger::Verbose(std::format("[CA] Lost connection from {}:{}: {}",
                                address.ip, address.port, errorEvent.what()));
  }

  Shutdown();
}

/**
 * Handles a datagram incoming from the client.
 * @param dg
 */
void ClientParticipant::HandleClientDatagram(
    const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Metrics.
  _clientAgent->RecordDatagram(dg->Size());

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

#ifdef ARDOS_USE_LEGACY_CLIENT
  // A successfully handled message in legacy mode doubles as a heartbeat
  // message. (Fairies automatically detects quiet periods and will send a
  // standard heartbeat message out.)
  HandleClientHeartbeat();
#endif
}

/**
 * Handles a datagram incoming from the Message Director.
 * @param dg
 */
void ClientParticipant::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);
  dgi.SeekPayload();

  uint64_t sender = dgi.GetUint64();
  if (sender == _channel) {
    // Ignore loopback messages.
    return;
  }

  uint16_t msgType = dgi.GetUint16();
  switch (msgType) {
  case CLIENTAGENT_EJECT: {
    uint16_t reason = dgi.GetUint16();
    std::string message = dgi.GetString();
    SendDisconnect(reason, message);
    break;
  }
  case CLIENTAGENT_DROP: {
    _cleanDisconnect = true;
    NetworkClient::Shutdown();
    break;
  }
  case CLIENTAGENT_SET_STATE:
    _authState = (AuthState)dgi.GetUint16();
    break;
  case CLIENTAGENT_ADD_INTEREST: {
    uint32_t context = _nextContext++;

    Interest i;
    BuildInterest(dgi, false, i);
    HandleAddInterest(i, context);
    AddInterest(i, context, sender);
  }
  case CLIENTAGENT_ADD_INTEREST_MULTIPLE: {
    uint32_t context = _nextContext++;

    Interest i;
    BuildInterest(dgi, true, i);
    HandleAddInterest(i, context);
    AddInterest(i, context, sender);
    break;
  }
  case CLIENTAGENT_REMOVE_INTEREST: {
    uint32_t context = _nextContext++;

    uint16_t id = dgi.GetUint16();
    Interest &i = _interests[id];
    HandleRemoveInterest(id, context);
    RemoveInterest(i, context, sender);
    break;
  }
  case CLIENTAGENT_SET_CLIENT_ID: {
    if (_channel != _allocatedChannel) {
      UnsubscribeChannel(_channel);
    }

    _channel = dgi.GetUint64();
    SubscribeChannel(_channel);
    break;
  }
  case CLIENTAGENT_SEND_DATAGRAM: {
    auto forwardDg = std::make_shared<Datagram>();
    forwardDg->AddData(dgi.GetRemainingBytes());
    SendDatagram(forwardDg);
    break;
  }
  case CLIENTAGENT_OPEN_CHANNEL:
    SubscribeChannel(dgi.GetUint64());
    break;
  case CLIENTAGENT_CLOSE_CHANNEL:
    UnsubscribeChannel(dgi.GetUint64());
    break;
  case CLIENTAGENT_ADD_POST_REMOVE:
    // TODO.
    break;
  case CLIENTAGENT_CLEAR_POST_REMOVES:
    // TODO.
    break;
  case CLIENTAGENT_DECLARE_OBJECT: {
    uint32_t doId = dgi.GetUint32();
    uint16_t dcId = dgi.GetUint16();

    if (_declaredObjects.contains(doId)) {
      Logger::Warn(std::format(
          "[CA] Client: {} received duplicate object declaration: {}", _channel,
          doId));
      break;
    }

    DeclaredObject obj{};
    obj.doId = doId;
    obj.dcc = g_dc_file->get_class(dcId);
    _declaredObjects[doId] = obj;
    break;
  }
  case CLIENTAGENT_UNDECLARE_OBJECT: {
    uint32_t doId = dgi.GetUint32();
    if (!_declaredObjects.contains(doId)) {
      Logger::Warn(std::format(
          "[CA] Client: {} received un-declare object for unknown DoId: ",
          _channel, doId));
      break;
    }

    _declaredObjects.erase(doId);
    break;
  }
  case CLIENTAGENT_SET_FIELDS_SENDABLE: {
    uint32_t doId = dgi.GetUint32();
    uint16_t fieldCount = dgi.GetUint16();

    std::unordered_set<uint16_t> fields;
    for (uint16_t i = 0; i < fieldCount; ++i) {
      fields.insert(dgi.GetUint16());
    }

    _fieldsSendable[doId] = fields;
    break;
  }
  case CLIENTAGENT_ADD_SESSION_OBJECT: {
    uint32_t doId = dgi.GetUint32();
    if (_sessionObjects.contains(doId)) {
      Logger::Warn(std::format(
          "[CA] Client: {} received duplicate session object declaration: {}",
          _channel, doId));
      break;
    }

    Logger::Verbose(std::format("[CA] Client: {} added session object: {}",
                                _channel, doId));

    _sessionObjects.insert(doId);
    break;
  }
  case CLIENTAGENT_REMOVE_SESSION_OBJECT: {
    uint32_t doId = dgi.GetUint32();
    if (!_sessionObjects.contains(doId)) {
      Logger::Warn(std::format(
          "[CA] Client: {} received remove session object for unknown DoId: {}",
          _channel, doId));
      break;
    }

    Logger::Verbose(
        std::format("[CA] Client: {} removed session object with DoId: {}",
                    _channel, doId));

    _sessionObjects.erase(doId);
    break;
  }
  case CLIENTAGENT_GET_TLVS_RESP: {
    // TODO.
    break;
  }
  case CLIENTAGENT_GET_NETWORK_ADDRESS: {
    // TODO.
    break;
  }
  case STATESERVER_OBJECT_SET_FIELD: {
    uint32_t doId = dgi.GetUint32();
    if (!LookupObject(doId)) {
      if (TryQueuePending(doId, dgi.GetUnderlyingDatagram())) {
        return;
      }

      Logger::Warn(std::format("[CA] Client: {} received server-side field "
                               "update for unknown object: {}",
                               _channel, doId));
      return;
    }

    if (sender != _channel) {
      uint16_t fieldId = dgi.GetUint16();
      HandleSetField(doId, fieldId, dgi);
    }
    break;
  }
  case STATESERVER_OBJECT_SET_FIELDS: {
    uint32_t doId = dgi.GetUint32();
    if (!LookupObject(doId)) {
      if (TryQueuePending(doId, dgi.GetUnderlyingDatagram())) {
        return;
      }

      Logger::Warn(
          std::format("[CA] Client: {} received server-side multi-field "
                      "update for unknown object: {}",
                      _channel, doId));
      return;
    }

    if (sender != _channel) {
      uint16_t numFields = dgi.GetUint16();
      HandleSetFields(doId, numFields, dgi);
    }
    break;
  }
  case STATESERVER_OBJECT_DELETE_RAM: {
    uint32_t doId = dgi.GetUint32();

    Logger::Verbose(std::format(
        "[CA] Client: {} received DeleteRam for object with DoId: {}", _channel,
        doId));

    if (!LookupObject(doId)) {
      if (TryQueuePending(doId, dgi.GetUnderlyingDatagram())) {
        return;
      }

      Logger::Warn(std::format(
          "[CA] Client: {} received server-side delete for unknown object: {}",
          _channel, doId));
      return;
    }

    if (_sessionObjects.contains(doId)) {
      // Erase the object from our session objects, otherwise it'll be deleted
      // again when we disconnect.
      _sessionObjects.erase(doId);

      SendDisconnect(
          CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
          std::format(
              "The session object with DoId: {} has been unexpectedly deleted",
              doId));
      return;
    }

    if (_seenObjects.contains(doId)) {
      HandleRemoveObject(doId);
      _seenObjects.erase(doId);
    }

    if (_ownedObjects.contains(doId)) {
      HandleRemoveOwnership(doId);
      _ownedObjects.erase(doId);
    }

    _historicalObjects.insert(doId);
    _visibleObjects.erase(doId);
    break;
  }
  case STATESERVER_OBJECT_ENTER_OWNER_WITH_REQUIRED:
  case STATESERVER_OBJECT_ENTER_OWNER_WITH_REQUIRED_OTHER: {
    uint32_t doId = dgi.GetUint32();
    uint32_t parent = dgi.GetUint32();
    uint32_t zone = dgi.GetUint32();
    uint16_t dcId = dgi.GetUint16();

    if (!_ownedObjects.contains(doId)) {
      OwnedObject obj{};
      obj.doId = doId;
      obj.parent = parent;
      obj.zone = zone;
      obj.dcc = g_dc_file->get_class(dcId);
      _ownedObjects[doId] = obj;
    }

    bool withOther =
        (msgType == STATESERVER_OBJECT_ENTER_OWNER_WITH_REQUIRED_OTHER);
    HandleAddOwnership(doId, parent, zone, dcId, dgi, withOther);
    break;
  }
  case STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED:
  case STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED_OTHER: {
    uint32_t doId = dgi.GetUint32();
    uint32_t parent = dgi.GetUint32();
    uint32_t zone = dgi.GetUint32();

    for (const auto &it : _pendingInterests) {
      InterestOperation *iop = it.second;
      if (iop->_parent == parent && iop->_zones.contains(zone)) {
        iop->QueueDatagram(dgi.GetUnderlyingDatagram());
        _pendingObjects.emplace(doId, it.first);
        return;
      }
    }

    // Object entrance doesn't pertain to any pending interest operation,
    // so seek back to where we started and handle it normally.
    dgi.SeekPayload();
    dgi.Skip(sizeof(uint64_t) + sizeof(uint16_t)); // Sender + MsgType.

    bool withOther =
        (msgType == STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED_OTHER);
    HandleObjectEntrance(dgi, withOther);
    break;
  }
  case STATESERVER_OBJECT_ENTER_INTEREST_WITH_REQUIRED:
  case STATESERVER_OBJECT_ENTER_INTEREST_WITH_REQUIRED_OTHER: {
    uint32_t requestContext = dgi.GetUint32();
    auto it = _pendingInterests.find(requestContext);
    if (it == _pendingInterests.end()) {
      Logger::Warn(std::format("[CA] Client: {} received object entrance into "
                               "interest with unknown context: {}",
                               _channel, requestContext));
      return;
    }

    _pendingObjects[dgi.GetUint32()] = requestContext;

    it->second->QueueExpected(dgi.GetUnderlyingDatagram());
    if (it->second->IsReady()) {
      it->second->Finish();
    }
    break;
  }
  case STATESERVER_OBJECT_GET_ZONES_COUNT_RESP: {
    uint32_t context = dgi.GetUint32();
    uint32_t count = dgi.GetUint32();

    auto it = _pendingInterests.find(context);
    if (it == _pendingInterests.end()) {
      Logger::Error(std::format(
          "[CA] Client: {} received GET_ZONES_COUNT for unknown context: {}",
          _channel, context));
      return;
    }

    it->second->SetExpected(count);
    if (it->second->IsReady()) {
      it->second->Finish();
    }
    break;
  }
  case STATESERVER_OBJECT_CHANGING_LOCATION: {
    uint32_t doId = dgi.GetUint32();
    if (TryQueuePending(doId, dgi.GetUnderlyingDatagram())) {
      return;
    }

    uint32_t newParent = dgi.GetUint32();
    uint32_t newZone = dgi.GetUint32();

    bool disable = true;
    for (const auto &it : _interests) {
      const Interest &i = it.second;
      if (i.parent == newParent) {
        for (const auto &it2 : i.zones) {
          if (it2 == newZone) {
            disable = false;
            break;
          }
        }
      }
    }

    bool visible = _visibleObjects.contains(doId);
    bool owned = _ownedObjects.contains(doId);

    if (!visible && !owned) {
      // We don't actually *see* this object, we're receiving this message as a
      // fluke.
      return;
    }

    bool session = _sessionObjects.contains(doId);

    if (visible) {
      _visibleObjects[doId].parent = newParent;
      _visibleObjects[doId].zone = newZone;
    }

    if (owned) {
      _ownedObjects[doId].parent = newParent;
      _ownedObjects[doId].zone = newZone;
    }

    // Disable this object if:
    // 1 - We don't have interest in its location (i.e. disable == true)
    // 2 - It's visible (owned objects may exist without being visible.)
    // 3 - If it's owned, it isn't a session object
    if (disable && visible) {
      if (session) {
        if (owned) {
          // Owned session object: do not disable, but send
          // CLIENT_OBJECT_LOCATION
          HandleChangeLocation(doId, newParent, newZone);
        } else {
          SendDisconnect(CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
                         std::format("The session object with id: {} has "
                                     "unexpectedly left interest",
                                     doId));
        }
        return;
      }

      HandleRemoveObject(doId);
      _seenObjects.erase(doId);
      _historicalObjects.insert(doId);
      _visibleObjects.erase(doId);
    } else {
      HandleChangeLocation(doId, newParent, newZone);
    }
    break;
  }
  case STATESERVER_OBJECT_CHANGING_OWNER: {
    uint32_t doId = dgi.GetUint32();
    uint64_t newOwner = dgi.GetUint64();

    // Don't care about the old owner.
    dgi.Skip(sizeof(uint64_t));

    if (newOwner == _channel) {
      // We should already own this object, nothing changes, and we might get
      // another enter owner message.
      return;
    }

    if (!_ownedObjects.contains(doId)) {
      Logger::Error(std::format(
          "[CA] Client: {} received changing owner for unowned object: {}",
          _channel, doId));
      return;
    }

    // If it's a session object, disconnect the client.
    if (_sessionObjects.contains(doId)) {
      SendDisconnect(
          CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
          std::format(
              "The session object with id: {} has unexpectedly left ownership",
              doId));
      return;
    }

    HandleRemoveOwnership(doId);
    _ownedObjects.erase(doId);
    break;
  }
  default:
    Logger::Error(std::format("Client: {} received unknown MsgType: {}",
                              _channel, msgType));
  }
}

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

  // This will call Shutdown from HandleDisconnect.
  _cleanDisconnect = true;
  Shutdown();
}

/**
 * Resets this client's heartbeat disconnect timer.
 */
void ClientParticipant::HandleClientHeartbeat() {
  if (_heartbeatTimer) {
    _heartbeatTimer->stop();
    _heartbeatTimer->start(
        uvw::timer_handle::time{_clientAgent->GetHeartbeatInterval()},
        uvw::timer_handle::time{0});
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

  // Get the configured shim UberDOG.
  DCClass *authClass = LookupObject(authShim);
  if (!authClass) {
    Logger::Error(std::format(
        "[CA] Auth shim DoId: {} is not a configured UberDOG", authShim));
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "No available login handler!");
    return;
  }

  // Get the login handler id. This is a generic handle for all Disney login
  // messages.
  DCField *authField = authClass->get_field_by_name("login");
  if (!authField) {
    Logger::Error(std::format(
        "[CA] Auth shim UberDOG: {} does not define a login field", authShim));
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

  _authState = AUTH_STATE_ANONYMOUS;

  // We've got a matching version and hash, send off the login request to the
  // configured shim UberDOG!
  auto dg = std::make_shared<Datagram>(authShim, _channel,
                                       STATESERVER_OBJECT_SET_FIELD);
  dg->AddUint32(authShim);
  dg->AddUint16(authField->get_number());
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

  Interest i;

#ifdef ARDOS_USE_LEGACY_CLIENT
  uint16_t handleId = dgi.GetUint16();
  uint32_t context = dgi.GetUint32();
  BuildInterest(dgi, multiple, i, handleId);
#else
  uint32_t context = dgi.GetUint32();
  BuildInterest(dgi, multiple, i);
#endif

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

void ClientParticipant::HandleClientRemoveInterest(DatagramIterator &dgi) {
  if (_clientAgent->GetInterestsPermission() == INTERESTS_DISABLED) {
    SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
                   "Client is not allowed to remove interests", true);
    return;
  }

  uint32_t context = dgi.GetUint32();
  uint16_t id = dgi.GetUint16();

  // Make sure the interest actually exists.
  if (!_interests.contains(id)) {
    SendDisconnect(
        CLIENT_DISCONNECT_GENERIC,
        std::format("Tried to remove a non-existent interest: {}", id), true);
    return;
  }

  Interest &i = _interests[id];
  if (_clientAgent->GetInterestsPermission() == INTERESTS_VISIBLE &&
      !LookupObject(i.parent)) {
    SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
                   std::format("Cannot remove interest for parent: {} because "
                               "parent is not visible to client",
                               i.parent),
                   true);
    return;
  }

  RemoveInterest(i, context);
}

void ClientParticipant::BuildInterest(DatagramIterator &dgi,
                                      const bool &multiple, Interest &out,
                                      const uint16_t &handleId) {
#ifdef ARDOS_USE_LEGACY_CLIENT
  uint16_t interestId = handleId;
#else
  uint16_t interestId = dgi.GetUint16();
#endif
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

void ClientParticipant::NotifyInterestDone(const InterestOperation *iop) {
  if (iop->_callers.empty()) {
    return;
  }

  auto dg = std::make_shared<Datagram>(iop->_callers, _channel,
                                       CLIENTAGENT_DONE_INTEREST_RESP);
  dg->AddUint64(_channel);
  dg->AddUint16(iop->_interestId);
  PublishDatagram(dg);
}

void ClientParticipant::HandleInterestDone(const uint16_t &interestId,
                                           const uint32_t &context) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_DONE_INTEREST_RESP);
#ifdef ARDOS_USE_LEGACY_CLIENT
  dg->AddUint16(interestId);
  dg->AddUint32(context);
#else
  dg->AddUint32(context);
  dg->AddUint16(interestId);
#endif
  SendDatagram(dg);
}

void ClientParticipant::HandleAddInterest(const Interest &i,
                                          const uint32_t &context) {
  bool multiple = i.zones.empty();

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(multiple ? CLIENT_ADD_INTEREST : CLIENT_ADD_INTEREST_MULTIPLE);
  dg->AddUint32(context);
  dg->AddUint16(i.id);
  dg->AddUint32(i.parent);
  if (multiple) {
    dg->AddUint16(i.zones.size());
  }
  for (const auto &zone : i.zones) {
    dg->AddUint32(zone);
  }
  SendDatagram(dg);
}

void ClientParticipant::HandleRemoveInterest(const uint16_t &interestId,
                                             const uint32_t &context) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_REMOVE_INTEREST);
  dg->AddUint32(context);
  dg->AddUint16(interestId);
  SendDatagram(dg);
}

void ClientParticipant::RemoveInterest(Interest &i, const uint32_t &context,
                                       const uint64_t &caller) {
  std::unordered_set<uint32_t> killedZones;
  for (const auto &zone : i.zones) {
    if (LookupInterests(i.parent, zone).size() == 1) {
      // We're the only interest who can see this zone, so let's kill it.
      killedZones.insert(zone);
    }
  }

  // Now that we know which zones to kill, let's get to it.
  CloseZones(i.parent, killedZones);

  NotifyInterestDone(i.id, caller);
  HandleInterestDone(i.id, context);

  _interests.erase(i.id);
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

void ClientParticipant::HandleRemoveOwnership(const uint32_t &doId) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_LEAVING_OWNER);
  dg->AddUint32(doId);
  SendDatagram(dg);
}

void ClientParticipant::HandleObjectEntrance(DatagramIterator &dgi,
                                             const bool &other) {
  uint32_t doId = dgi.GetUint32();
  uint32_t parent = dgi.GetUint32();
  uint32_t zone = dgi.GetUint32();
  uint16_t dcId = dgi.GetUint16();

  // This object is no longer pending.
  _pendingObjects.erase(doId);

  if (_seenObjects.contains(doId)) {
    return;
  }

  if (_ownedObjects.contains(doId) && _sessionObjects.contains(doId)) {
    return;
  }

  if (!_visibleObjects.contains(doId)) {
    VisibleObject obj{};
    obj.doId = doId;
    obj.dcc = g_dc_file->get_class(dcId);
    obj.parent = parent;
    obj.zone = zone;
    _visibleObjects[doId] = obj;
  }

  _seenObjects.insert(doId);

  HandleAddObject(doId, parent, zone, dcId, dgi, other);
}

void ClientParticipant::HandleAddObject(
    const uint32_t &doId, const uint32_t &parentId, const uint32_t &zoneId,
    const uint16_t &dcId, DatagramIterator &dgi, const bool &other) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(other ? CLIENT_ENTER_OBJECT_REQUIRED_OTHER
                      : CLIENT_ENTER_OBJECT_REQUIRED);
  Logger::Info(std::format("Send entry: {}", doId));
#ifdef ARDOS_USE_LEGACY_CLIENT
  // TODO: Check if this is the same for Toontown/Pirates.
  dg->AddLocation(parentId, zoneId);
  dg->AddUint16(dcId);
  dg->AddUint32(doId);
#else
  dg->AddUint32(doId);
  dg->AddLocation(parentId, zoneId);
  dg->AddUint16(dcId);
#endif
  dg->AddData(dgi.GetRemainingBytes());
  SendDatagram(dg);
}

bool ClientParticipant::TryQueuePending(const uint32_t &doId,
                                        const std::shared_ptr<Datagram> &dg) {
  auto it = _pendingObjects.find(doId);
  if (it != _pendingObjects.end()) {
    // The datagram should be queued under the appropriate interest operation.
    _pendingInterests.find(it->second)->second->QueueDatagram(dg);
    return true;
  }

  // We have no idea what DoId is being talked about.
  return false;
}

void ClientParticipant::HandleSetField(const uint32_t &doId,
                                       const uint16_t &fieldId,
                                       DatagramIterator &dgi) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_SET_FIELD);
  dg->AddUint32(doId);
  dg->AddUint16(fieldId);
  dg->AddData(dgi.GetRemainingBytes());
  SendDatagram(dg);
}

void ClientParticipant::HandleSetFields(const uint32_t &doId,
                                        const uint16_t &numFields,
                                        DatagramIterator &dgi) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_SET_FIELDS);
  dg->AddUint32(doId);
  dg->AddUint16(numFields);
  dg->AddData(dgi.GetRemainingBytes());
  SendDatagram(dg);
}

void ClientParticipant::HandleAddOwnership(
    const uint32_t &doId, const uint32_t &parentId, const uint32_t &zoneId,
    const uint16_t &dcId, DatagramIterator &dgi, const bool &other) {
  auto dg = std::make_shared<Datagram>();
#ifdef ARDOS_USE_LEGACY_CLIENT
  // Fairies only accepts OTHER_OWNER entries and has a slightly different data
  // order.
  // TODO: Check if this is the same for Toontown/Pirates.
  dg->AddUint16(CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER);
  dg->AddUint16(dcId);
  dg->AddUint32(doId);
  dg->AddLocation(parentId, zoneId);
#else
  dg->AddUint16(other ? CLIENT_ENTER_OBJECT_REQUIRED_OTHER_OWNER
                      : CLIENT_ENTER_OBJECT_REQUIRED_OWNER);
  dg->AddUint32(doId);
  dg->AddLocation(parentId, zoneId);
  dg->AddUint16(dcId);
#endif
  dg->AddData(dgi.GetRemainingBytes());
  SendDatagram(dg);
}

void ClientParticipant::HandleChangeLocation(const uint32_t &doId,
                                             const uint32_t &newParent,
                                             const uint32_t &newZone) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_LOCATION);
  dg->AddUint32(doId);
  dg->AddLocation(newParent, newZone);
  SendDatagram(dg);
}

} // namespace Ardos
