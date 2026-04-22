#include "client_participant.h"

#include "../net/message_types.h"
#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

static bool IsClassOrDerivedFrom(const DCClass* candidate, DCClass* baseClass) {
  if (!candidate || !baseClass) {
    return false;
  }
  if (candidate == baseClass) {
    return true;
  }

  const int parentCount = candidate->get_num_parents();
  for (int i = 0; i < parentCount; ++i) {
    if (IsClassOrDerivedFrom(candidate->get_parent(i), baseClass)) {
      return true;
    }
  }

  return false;
}

ClientParticipant::ClientParticipant(
    ClientAgent* clientAgent, const std::shared_ptr<uvw::tcp_handle>& socket)
    : NetworkClient(socket), _clientAgent(clientAgent) {
  auto address = GetRemoteAddress();
  spdlog::get("ca")->debug("Client connected from {}:{}", address.ip,
                           address.port);

  _channel = _clientAgent->AllocateChannel();
  if (!_channel) {
    spdlog::get("ca")->error("Channel range depleted!");
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
        [this](const uvw::timer_event&, uvw::timer_handle&) {
          HandleHeartbeatTimeout();
        });
  }

  if (_clientAgent->GetAuthTimeout()) {
    // Set up the auth timeout timer.
    _authTimer = g_loop->resource<uvw::timer_handle>();
    _authTimer->on<uvw::timer_event>(
        [this](const uvw::timer_event&, uvw::timer_handle&) {
          HandleAuthTimeout();
        });

    _authTimer->start(uvw::timer_handle::time{_clientAgent->GetAuthTimeout()},
                      uvw::timer_handle::time{0});
  }

  if (_clientAgent->GetHistoricalTTL()) {
    // Set up the historical object TTL timer.
    _historicalTimer = g_loop->resource<uvw::timer_handle>();
    _historicalTimer->on<uvw::timer_event>(
        [this](const uvw::timer_event&, uvw::timer_handle&) {
          CleanupHistorical();
        });

    // Sweep at 2 second intervals.
    _historicalTimer->start(uvw::timer_handle::time{2000},
                            uvw::timer_handle::time{2000});
  }

  _clientAgent->ParticipantJoined();
}

ClientParticipant::~ClientParticipant() {
  // Call shutdown just in-case (most likely redundant.)
  Shutdown();

  _clientAgent->ParticipantLeft(this);
}

/**
 * Manually disconnect and delete this client participant.
 */
void ClientParticipant::Shutdown() {
  if (_disconnected) {
    return;
  }

  // Kill the network connection.
  NetworkClient::Shutdown();

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

  // Stop the historical timer (if we have one.)
  if (_historicalTimer) {
    _historicalTimer->stop();
    _historicalTimer->close();
    _historicalTimer.reset();
  }

  // Unsubscribe from all channels so DELETE messages aren't sent back to us.
  ChannelSubscriber::Shutdown();
  _clientAgent->FreeChannel(_allocatedChannel);

  // Delete all session objects.
  while (!_sessionObjects.empty()) {
    uint32_t doId = *_sessionObjects.begin();
    _sessionObjects.erase(doId);
    spdlog::get("ca")->debug("Client: {} exited, deleting session object: {}",
                             _channel, doId);

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

  spdlog::get("ca")->debug("Routing {} post-remove(s) for '{}'",
                           _postRemoves.size(), _channel);

  // Route any post remove datagrams we might have stored.
  for (const auto& dg : _postRemoves) {
    PublishDatagram(dg);
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
    spdlog::get("ca")->debug("Lost connection from {}:{}: {}", address.ip,
                             address.port, errorEvent.what());
  }

  Shutdown();
}

/**
 * Handles a datagram incoming from the Message Director.
 * @param dg
 */
void ClientParticipant::HandleDatagram(const std::shared_ptr<Datagram>& dg) {
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
      break;
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
      Interest& i = _interests[id];
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
      _postRemoves.emplace_back(dgi.GetDatagram());
      break;
    case CLIENTAGENT_CLEAR_POST_REMOVES:
      _postRemoves.clear();
      break;
    case CLIENTAGENT_DECLARE_OBJECT: {
      uint32_t doId = dgi.GetUint32();
      uint16_t dcId = dgi.GetUint16();

      if (_declaredObjects.contains(doId)) {
        spdlog::get("ca")->warn(
            "Client: {} received duplicate object declaration: {}", _channel,
            doId);
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
        spdlog::get("ca")->warn(
            "Client: {} received un-declare object for unknown DoId: ",
            _channel, doId);
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
        spdlog::get("ca")->warn(
            "Client: {} received duplicate session object declaration: {}",
            _channel, doId);
        break;
      }

      spdlog::get("ca")->debug("Client: {} added session object: {}", _channel,
                               doId);

      _sessionObjects.insert(doId);
      break;
    }
    case CLIENTAGENT_REMOVE_SESSION_OBJECT: {
      uint32_t doId = dgi.GetUint32();
      if (!_sessionObjects.contains(doId)) {
        spdlog::get("ca")->warn(
            "Client: {} received remove session object for unknown DoId: {}",
            _channel, doId);
        break;
      }

      spdlog::get("ca")->debug(
          "Client: {} removed session object with DoId: {}", _channel, doId);

      _sessionObjects.erase(doId);
      break;
    }
    case CLIENTAGENT_GET_NETWORK_ADDRESS: {
      auto resp = std::make_shared<Datagram>(
          sender, _channel, CLIENTAGENT_GET_NETWORK_ADDRESS_RESP);
      resp->AddUint32(dgi.GetUint32());  // Context.
      resp->AddString(GetRemoteAddress().ip);
      resp->AddUint16(GetRemoteAddress().port);
      resp->AddString(GetLocalAddress().ip);
      resp->AddUint16(GetLocalAddress().port);
      PublishDatagram(resp);
      break;
    }
    case STATESERVER_OBJECT_SET_FIELD: {
      uint32_t doId = dgi.GetUint32();
      if (!LookupObject(doId)) {
        if (TryQueuePending(doId, dgi.GetUnderlyingDatagram())) {
          return;
        }

        spdlog::get("ca")->warn(
            "Client: {} received server-side field "
            "update for unknown object: {}",
            _channel, doId);
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

        spdlog::get("ca")->warn(
            "Client: {} received server-side multi-field "
            "update for unknown object: {}",
            _channel, doId);
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

      spdlog::get("ca")->debug(
          "Client: {} received DeleteRam for object with DoId: {}", _channel,
          doId);

      if (!LookupObject(doId)) {
        if (TryQueuePending(doId, dgi.GetUnderlyingDatagram())) {
          return;
        }

        spdlog::get("ca")->warn(
            "Client: {} received server-side delete for unknown object: {}",
            _channel, doId);
        return;
      }

      if (_sessionObjects.contains(doId)) {
        // Erase the object from our session objects, otherwise it'll be deleted
        // again when we disconnect.
        _sessionObjects.erase(doId);

        SendDisconnect(CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
                       std::format("The session object with DoId: {} has been "
                                   "unexpectedly deleted",
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

      _historicalObjects[doId] = now_ms() + _clientAgent->GetHistoricalTTL();
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

      for (const auto& it : _pendingInterests) {
        InterestOperation* iop = it.second;
        if (iop->_parent == parent && iop->_zones.contains(zone)) {
          iop->QueueDatagram(dgi.GetUnderlyingDatagram());
          _pendingObjects.emplace(doId, it.first);
          return;
        }
      }

      // Object entrance doesn't pertain to any pending interest operation,
      // so seek back to where we started and handle it normally.
      dgi.SeekPayload();
      dgi.Skip(sizeof(uint64_t) + sizeof(uint16_t));  // Sender + MsgType.

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
        spdlog::get("ca")->warn(
            "Client: {} received object entrance into "
            "interest with unknown context: {}",
            _channel, requestContext);
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
        spdlog::get("ca")->error(
            "Client: {} received GET_ZONES_COUNT for unknown context: {}",
            _channel, context);
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
        // The object that's changing location is currently generating inside an
        // active InterestOperation. Queue this message to be handled after it
        // generates.
        return;
      }

      uint32_t newParent = dgi.GetUint32();
      uint32_t newZone = dgi.GetUint32();

      bool disable = true;
      for (const auto& it : _interests) {
        const Interest& i = it.second;
        if (i.parent == newParent) {
          for (const auto& it2 : i.zones) {
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
        // We don't actually *see* this object, we're receiving this message as
        // a fluke.
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
        _historicalObjects[doId] = now_ms() + _clientAgent->GetHistoricalTTL();
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
        spdlog::get("ca")->error(
            "Client: {} received changing owner for unowned object: {}",
            _channel, doId);
        return;
      }

      // If it's a session object, disconnect the client.
      if (_sessionObjects.contains(doId)) {
        SendDisconnect(CLIENT_DISCONNECT_SESSION_OBJECT_DELETED,
                       std::format("The session object with id: {} has "
                                   "unexpectedly left ownership",
                                   doId));
        return;
      }

      HandleRemoveOwnership(doId);
      _ownedObjects.erase(doId);
      break;
    }
    default:
      spdlog::get("ca")->error("Client: {} received unknown MsgType: {}",
                               _channel, msgType);
  }
}

/**
 * Disconnects this client with a reason and message.
 * @param reason
 * @param message
 * @param security
 */
void ClientParticipant::SendDisconnect(const uint16_t& reason,
                                       const std::string& message,
                                       const bool& security) {
  if (Disconnected()) {
    return;
  }

  security ? spdlog::get("ca")->error("Ejecting client: '{}': {} - {}",
                                      _channel, reason, message)
           : spdlog::get("ca")->warn("Ejecting client: '{}': {} - {}", _channel,
                                     reason, message);

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
 * Expire historical-object TTL entries whose deadline has passed.
 */
void ClientParticipant::CleanupHistorical() {
  const uint64_t now = now_ms();

  for (auto it = _historicalObjects.begin(); it != _historicalObjects.end();) {
    if (it->second <= now) {
      it = _historicalObjects.erase(it);
    } else {
      ++it;
    }
  }
}

/**
 * Lookup a Distributed Object in-view of this client and return its class.
 * @param doId
 */
DCClass* ClientParticipant::LookupObject(const uint32_t& doId) {
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

void ClientParticipant::HandleRemoveObject(const uint32_t& doId) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_LEAVING);
  dg->AddUint32(doId);
  SendDatagram(dg);
}

void ClientParticipant::HandleRemoveOwnership(const uint32_t& doId) {
  if (_avatarDoId == doId) {
    _avatarDoId = INVALID_DO_ID;
    _avatarParent = INVALID_DO_ID;
    _avatarZone = INVALID_DO_ID;
  }

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_LEAVING_OWNER);
  dg->AddUint32(doId);
  SendDatagram(dg);
}

void ClientParticipant::HandleObjectEntrance(DatagramIterator& dgi,
                                             const bool& other) {
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
    const uint32_t& doId, const uint32_t& parentId, const uint32_t& zoneId,
    const uint16_t& dcId, DatagramIterator& dgi, const bool& other) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(other ? CLIENT_ENTER_OBJECT_REQUIRED_OTHER
                      : CLIENT_ENTER_OBJECT_REQUIRED);
  spdlog::get("ca")->debug("Sending entry of object: {} to client: {}", doId,
                           _channel);
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

bool ClientParticipant::TryQueuePending(const uint32_t& doId,
                                        const std::shared_ptr<Datagram>& dg) {
  auto it = _pendingObjects.find(doId);
  if (it != _pendingObjects.end()) {
    // The datagram should be queued under the appropriate interest operation.
    _pendingInterests.find(it->second)->second->QueueDatagram(dg);
    return true;
  }

  // We have no idea what DoId is being talked about.
  return false;
}

void ClientParticipant::HandleSetField(const uint32_t& doId,
                                       const uint16_t& fieldId,
                                       DatagramIterator& dgi) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_SET_FIELD);
  dg->AddUint32(doId);
  dg->AddUint16(fieldId);
  dg->AddData(dgi.GetRemainingBytes());
  SendDatagram(dg);
}

void ClientParticipant::HandleSetFields(const uint32_t& doId,
                                        const uint16_t& numFields,
                                        DatagramIterator& dgi) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_SET_FIELDS);
  dg->AddUint32(doId);
  dg->AddUint16(numFields);
  dg->AddData(dgi.GetRemainingBytes());
  SendDatagram(dg);
}

void ClientParticipant::HandleAddOwnership(
    const uint32_t& doId, const uint32_t& parentId, const uint32_t& zoneId,
    const uint16_t& dcId, DatagramIterator& dgi, const bool& other) {
  // Track location from the configured avatar class only.
  DCClass* avatarClass = _clientAgent->GetAvatarClass();
  if (IsClassOrDerivedFrom(_ownedObjects[doId].dcc, avatarClass)) {
    _avatarDoId = doId;
    _avatarParent = parentId;
    _avatarZone = zoneId;
  }

  auto dg = std::make_shared<Datagram>();
  spdlog::get("ca")->debug("Sending owner entry of object: {} to client: {}",
                           doId, _channel);
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

void ClientParticipant::HandleChangeLocation(const uint32_t& doId,
                                             const uint32_t& newParent,
                                             const uint32_t& newZone) {
  // If the players avatar is changing location, make sure we keep track of it.
  if (_avatarDoId == doId) {
    _avatarParent = newParent;
    _avatarZone = newZone;
  }

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_OBJECT_LOCATION);
  dg->AddUint32(doId);
  dg->AddLocation(newParent, newZone);
  SendDatagram(dg);
}

}  // namespace Ardos
