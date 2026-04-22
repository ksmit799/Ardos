#include "../net/message_types.h"
#include "../util/logger.h"
#include "client_participant.h"

namespace Ardos {

static bool ZoneInConfiguredSet(const ClientAgent* agent, uint32_t zoneId) {
  if (agent->GetInterestZones().contains(zoneId)) {
    return true;
  }
  for (const auto& [lo, hi] : agent->GetInterestZoneRanges()) {
    if (zoneId >= lo && zoneId <= hi) {
      return true;
    }
  }
  return false;
}

void ClientParticipant::HandleClientAddInterest(DatagramIterator& dgi,
                                                const bool& multiple) {
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

  // Enforce interest zone whitelist/blacklist from config.
  const auto mode = _clientAgent->GetInterestMode();
  const bool hasZoneFilter = !_clientAgent->GetInterestZones().empty() ||
                             !_clientAgent->GetInterestZoneRanges().empty();

  if (hasZoneFilter && mode != INTEREST_MODE_NONE) {
    for (uint32_t zoneId : i.zones) {
      const bool inSet = ZoneInConfiguredSet(_clientAgent, zoneId);
      if (mode == INTEREST_MODE_WHITELIST && !inSet) {
        SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
                       std::format("Zone {} is not allowed by interest "
                                   "whitelist",
                                   zoneId),
                       true);
        return;
      }
      if (mode == INTEREST_MODE_BLACKLIST && inSet) {
        SendDisconnect(CLIENT_DISCONNECT_FORBIDDEN_INTEREST,
                       std::format("Zone {} is disallowed by interest "
                                   "blacklist",
                                   zoneId),
                       true);
        return;
      }
    }
  }

  AddInterest(i, context);
}

void ClientParticipant::HandleClientRemoveInterest(DatagramIterator& dgi) {
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

  Interest& i = _interests[id];
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

void ClientParticipant::BuildInterest(DatagramIterator& dgi,
                                      const bool& multiple, Interest& out,
                                      const uint16_t& handleId) {
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

void ClientParticipant::AddInterest(Interest& i, const uint32_t& context,
                                    const uint64_t& caller) {
  std::unordered_set<uint32_t> newZones;
  for (const auto& zone : i.zones) {
    if (LookupInterests(i.parent, zone).empty()) {
      newZones.insert(zone);
    }
  }

  // This is an already open interest handle that the client is modifying.
  if (_interests.contains(i.id)) {
    Interest previousInterest = _interests[i.id];

    std::unordered_set<uint32_t> killedZones;
    for (const auto& zone : previousInterest.zones) {
      if (LookupInterests(previousInterest.parent, zone).size() > 1) {
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
  for (const auto& zone : newZones) {
    dg->AddUint32(zone);
    SubscribeChannel(LocationAsChannel(i.parent, zone));
  }
  PublishDatagram(dg);
}

std::vector<Interest> ClientParticipant::LookupInterests(
    const uint32_t& parentId, const uint32_t& zoneId) {
  std::vector<Interest> interests;
  for (const auto& interest : _interests) {
    if (parentId == interest.second.parent &&
        interest.second.zones.contains(zoneId)) {
      interests.push_back(interest.second);
    }
  }

  return interests;
}

void ClientParticipant::NotifyInterestDone(const uint16_t& interestId,
                                           const uint64_t& caller) {
  if (!caller) {
    return;
  }

  auto dg = std::make_shared<Datagram>(caller, _channel,
                                       CLIENTAGENT_DONE_INTEREST_RESP);
  dg->AddUint64(_channel);
  dg->AddUint16(interestId);
  PublishDatagram(dg);
}

void ClientParticipant::NotifyInterestDone(const InterestOperation* iop) {
  if (iop->_callers.empty()) {
    return;
  }

  auto dg = std::make_shared<Datagram>(iop->_callers, _channel,
                                       CLIENTAGENT_DONE_INTEREST_RESP);
  dg->AddUint64(_channel);
  dg->AddUint16(iop->_interestId);
  PublishDatagram(dg);
}

void ClientParticipant::HandleInterestDone(const uint16_t& interestId,
                                           const uint32_t& context) {
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

void ClientParticipant::HandleAddInterest(const Interest& i,
                                          const uint32_t& context) {
  bool multiple = i.zones.empty();

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(multiple ? CLIENT_ADD_INTEREST : CLIENT_ADD_INTEREST_MULTIPLE);
  dg->AddUint32(context);
  dg->AddUint16(i.id);
  dg->AddUint32(i.parent);
  if (multiple) {
    dg->AddUint16(i.zones.size());
  }
  for (const auto& zone : i.zones) {
    dg->AddUint32(zone);
  }
  SendDatagram(dg);
}

void ClientParticipant::HandleRemoveInterest(const uint16_t& interestId,
                                             const uint32_t& context) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(CLIENT_REMOVE_INTEREST);
  dg->AddUint32(context);
  dg->AddUint16(interestId);
  SendDatagram(dg);
}

void ClientParticipant::RemoveInterest(Interest& i, const uint32_t& context,
                                       const uint64_t& caller) {
  std::unordered_set<uint32_t> killedZones;
  for (const auto& zone : i.zones) {
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
    const uint32_t& parent, const std::unordered_set<uint32_t>& killedZones) {
  for (const auto& zone : killedZones) {
    spdlog::get("ca")->debug("Client: {} closing zone: {} - {}", _channel,
                             parent, zone);
  }

  std::vector<uint32_t> toRemove;
  for (const auto& objects : _visibleObjects) {
    const VisibleObject& visibleObject = objects.second;
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
      _historicalObjects[visibleObject.doId] =
          now_ms() + _clientAgent->GetHistoricalTTL();
      toRemove.push_back(visibleObject.doId);
    }
  }

  for (const auto& doId : toRemove) {
    _visibleObjects.erase(doId);
  }

  // Close out killed channels.
  for (const auto& zone : killedZones) {
    UnsubscribeChannel(LocationAsChannel(parent, zone));
  }
}

}  // namespace Ardos
