#include <dcField.h>

#include "../net/message_types.h"
#include "../util/logger.h"
#include "client_participant.h"

namespace Ardos {

/**
 * Handles a datagram incoming from the client.
 * @param dg
 */
void ClientParticipant::HandleClientDatagram(
    const std::shared_ptr<Datagram>& dg) {
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
  } catch (const DatagramIteratorEOF&) {
    SendDisconnect(CLIENT_DISCONNECT_TRUNCATED_DATAGRAM,
                   "Datagram unexpectedly ended while iterating.");
    return;
  } catch (const DatagramOverflow&) {
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

void ClientParticipant::HandlePreHello(DatagramIterator& dgi) {
  uint16_t msgType = dgi.GetUint16();

#ifdef ARDOS_USE_LEGACY_CLIENT
  switch (msgType) {
    case CLIENT_LOGIN_FAIRIES:
    case CLIENT_LOGIN_TOONTOWN:
    case CLIENT_LOGIN_3:
      HandleLoginLegacy(dgi);
      break;
    case CLIENT_HEARTBEAT:
      HandleClientHeartbeat();
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
#endif  // ARDOS_USE_LEGACY_CLIENT
}

#ifdef ARDOS_USE_LEGACY_CLIENT
void ClientParticipant::HandleLoginLegacy(DatagramIterator& dgi) {
  uint32_t authShim = _clientAgent->GetAuthShim();
  if (!authShim) {
    spdlog::get("ca")->error("No configured auth shim for legacy login!");
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "No available login handler!");
    return;
  }

  // Get the configured shim UberDOG.
  DCClass* authClass = LookupObject(authShim);
  if (!authClass) {
    spdlog::get("ca")->error("Auth shim DoId: {} is not a configured UberDOG",
                             authShim);
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "No available login handler!");
    return;
  }

  // Get the login handler id. This is a generic handle for all Disney login
  // messages.
  DCField* authField = authClass->get_field_by_name("login");
  if (!authField) {
    spdlog::get("ca")->error(
        "Auth shim UberDOG: {} does not define a login field", authShim);
    SendDisconnect(CLIENT_DISCONNECT_GENERIC, "No available login handler!");
    return;
  }

  HandleClientHeartbeat();

  std::string loginToken = dgi.GetString();
  std::string clientVersion = dgi.GetString();
  uint32_t hashVal = dgi.GetUint32();
  // Skip remaining login data (this will be different per game.)
  // It's not necessary and if we don't skip it, the client will get
  // disconnected.
  dgi.Skip(dgi.GetRemainingSize());

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
#endif  // ARDOS_USE_LEGACY_CLIENT

void ClientParticipant::HandlePreAuth(DatagramIterator& dgi) {
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

void ClientParticipant::HandleAuthenticated(DatagramIterator& dgi) {
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
#ifdef ARDOS_USE_LEGACY_CLIENT
    case CLIENT_SET_AVTYPE:
      // Unneeded in legacy, each deployment should be game specific.
      dgi.GetUint16();  // Avatar dclass id.
      break;
#endif
    default:
      SendDisconnect(CLIENT_DISCONNECT_INVALID_MSGTYPE,
                     std::format("Client sent invalid message: {}", msgType),
                     true);
  }
}

/**
 * The client is attempting to update a field on a Distributed Object.
 * @param dgi
 */
void ClientParticipant::HandleClientObjectUpdateField(DatagramIterator& dgi) {
  uint32_t doId = dgi.GetUint32();
  uint16_t fieldId = dgi.GetUint16();

  DCClass* dcc = LookupObject(doId);
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
  DCField* field = dcc->get_field_by_index(fieldId);
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

  // Unpack the field data.
  std::vector<uint8_t> data;
  dgi.UnpackField(field, data);

  // Validate the field ranges (int16(0-200), etc.)
  if (!field->validate_ranges(data)) {
    SendDisconnect(CLIENT_DISCONNECT_FIELD_CONSTRAINT,
                   std::format("Client violated field constraints for "
                               "field: {} of class: {} (DoId: {})",
                               field->get_name(), dcc->get_name(), doId));
    return;
  }

#ifdef ARDOS_USE_LEGACY_CLIENT
  // Do we have a configured legacy chat shim?
  uint32_t chatShim = _clientAgent->GetChatShim();
  if (chatShim) {
    // We do, lets check if the field update relates to talking.
    if (field->get_name() == "setTalk" ||
        field->get_name() == "setTalkWhisper") {
      // It does, we have a TalkPath field update that requires filtering.

      // Get the configured shim UberDOG.
      DCClass* chatClass = LookupObject(chatShim);
      if (!chatClass) {
        spdlog::get("ca")->error(
            "Chat shim DoId: {} is not a configured UberDOG", chatShim);
        // Just drop the message to be safe.
        return;
      }

      // Get the field id for the specific talk message from the chat shim.
      // This is a 1-1 mapping with the TalkPath_ field names.
      DCField* chatField = chatClass->get_field_by_name(field->get_name());
      if (!chatField) {
        spdlog::get("ca")->error(
            "Chat shim UberDOG: {} does not define chat field: {}", chatShim,
            field->get_name());
        // Just drop the message to be safe.
        return;
      }

      // Send it off to the configured chat shim UberDOG.
      auto dg = std::make_shared<Datagram>(chatShim, _channel,
                                           STATESERVER_OBJECT_SET_FIELD);
      dg->AddUint32(chatShim);
      dg->AddUint16(chatField->get_number());
      // Send the clients avatar parentId/zoneId.
      dg->AddUint32(_avatarParent);
      dg->AddUint32(_avatarZone);
      // For whisper messages, add the target DoId (receiver.)
      if (field->get_name() == "setTalkWhisper") {
        dg->AddUint32(doId);
      }
      dg->AddData(data);
      PublishDatagram(dg);
      return;
    }
  }
#endif  // ARDOS_USE_LEGACY_CLIENT

  // Forward the field update to the state server.
  auto dg =
      std::make_shared<Datagram>(doId, _channel, STATESERVER_OBJECT_SET_FIELD);
  dg->AddUint32(doId);
  dg->AddUint16(fieldId);
  dg->AddData(data);
  PublishDatagram(dg);
}

void ClientParticipant::HandleClientObjectLocation(DatagramIterator& dgi) {
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

  auto parentId = dgi.GetUint32();
  auto zoneId = dgi.GetUint32();

  spdlog::get("ca")->debug(
      "Client: {} relocating owned object: {} to location: {}/{}", _channel,
      doId, parentId, zoneId);

  // Update the object's location with the state server.
  auto dg = std::make_shared<Datagram>(doId, _channel,
                                       STATESERVER_OBJECT_SET_LOCATION);
  dg->AddLocation(parentId, zoneId);
  PublishDatagram(dg);
}

}  // namespace Ardos
