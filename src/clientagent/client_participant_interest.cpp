#include <dcFile.h>

#include "../net/message_types.h"
#include "../util/globals.h"
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
  // Externally-triggered interests (client or AI) get Auto-rule extras
  // merged in before we touch any subscriptions. Rule-based internal
  // interests already know exactly what zones they want and don't need
  // further expansion.
  if (!i.isInternal) {
    MaybeApplyAutoRule(i);
  }

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

    if (!CloseZones(previousInterest.parent, killedZones)) {
      // This can happen if a zone containing one of our session objects is
      // closed. At this point, we'll be disconnected.
      return;
    }
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
  // CA-managed interests are invisible to the client; suppress the
  // completion notification. The client never saw a CLIENT_ADD_INTEREST
  // for this id, so a DONE_INTEREST_RESP would be confusing.
  auto it = _interests.find(interestId);
  if (it != _interests.end() && it->second.isInternal) {
    return;
  }

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
  const bool multiple = i.zones.size() != 1;

  auto dg = std::make_shared<Datagram>();
  dg->AddUint16(multiple ? CLIENT_ADD_INTEREST_MULTIPLE : CLIENT_ADD_INTEREST);
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
  if (!CloseZones(i.parent, killedZones)) {
    // This can happen if a zone containing one of our session objects is
    // closed. At this point, we'll be disconnected.
    return;
  }

  NotifyInterestDone(i.id, caller);
  HandleInterestDone(i.id, context);

  _interests.erase(i.id);
}

bool ClientParticipant::CloseZones(
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
        return false;
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

  return true;
}

void ClientParticipant::RequestParentClass(const uint32_t& parentId) {
  if (parentId == INVALID_DO_ID) {
    return;
  }

  uint32_t context = _nextContext++;

  auto& entry = _pendingParentClassLookups[context];
  entry.parentId = parentId;
  entry.timeout = g_loop->resource<uvw::timer_handle>();
  entry.timeout->on<uvw::timer_event>(
      [this, context](const uvw::timer_event&, uvw::timer_handle&) {
        HandleParentClassLookupTimeout(context);
      });
  entry.timeout->start(
      uvw::timer_handle::time{_clientAgent->GetInterestTimeout()},
      uvw::timer_handle::time{0});

  auto dg = std::make_shared<Datagram>(parentId, _channel,
                                       STATESERVER_OBJECT_GET_CLASS);
  dg->AddUint32(context);
  PublishDatagram(dg);
}

void ClientParticipant::HandleGetClassResp(DatagramIterator& dgi) {
  uint32_t context = dgi.GetUint32();
  uint32_t doId = dgi.GetUint32();
  uint16_t dcId = dgi.GetUint16();

  auto it = _pendingParentClassLookups.find(context);
  if (it == _pendingParentClassLookups.end()) {
    // Context unknown. Either we already timed it out (warning was logged
    // at that point) or it was never ours. Nothing to do.
    return;
  }

  uint32_t requestedParent = it->second.parentId;
  if (it->second.timeout) {
    it->second.timeout->stop();
    it->second.timeout->close();
    it->second.timeout.reset();
  }
  _pendingParentClassLookups.erase(it);

  // Defensive: response should be about the exact parent we asked for. A
  // mismatch here means something went wrong on the wire or contexts got
  // reused while one was still in flight -- refuse to act on it.
  if (doId != requestedParent) {
    spdlog::get("ca")->warn(
        "Client: {} got class response for doId {} but expected parent {} "
        "(ctx {})",
        _channel, doId, requestedParent, context);
    return;
  }

  // Stale check: the avatar may have left this parent (or logged out)
  // while the request was in flight. Dropping here is the right call --
  // if the avatar arrives back at the same parent later we'll have
  // already fired a fresh request.
  if (_avatarParent != requestedParent) {
    spdlog::get("ca")->debug(
        "Client: {} dropping stale class response for parent {} (avatar "
        "now under {})",
        _channel, requestedParent, _avatarParent);
    return;
  }

  DCClass* parentClass = g_dc_file->get_class(dcId);
  if (!parentClass) {
    spdlog::get("ca")->warn(
        "Client: {} got class response with unknown dcId {} for parent {}",
        _channel, dcId, requestedParent);
    return;
  }

  // Duplicate-resolution guard for the A->B->A scenario: both A lookups
  // can legitimately resolve, one after the other. The first sets
  // _avatarParentRule; the second sees it's already populated for this
  // parent and bails.
  if (_avatarParentRule && _avatarParentRule->parentId == requestedParent) {
    return;
  }

  ParentingRule rule;
  if (!TryParseParentingRule(parentClass, rule)) {
    // Parse failure or no rule => Stated-equivalent. Record the parent so
    // A->B->A dedupe still works, but don't open anything.
    _avatarParentRule = AvatarParentRule{requestedParent, ParentingRule{}, 0};
    return;
  }

  ApplyAvatarParentRule(requestedParent, rule);
}

void ClientParticipant::HandleParentClassLookupTimeout(
    const uint32_t& context) {
  auto it = _pendingParentClassLookups.find(context);
  if (it == _pendingParentClassLookups.end()) {
    // Shouldn't happen (the timer callback owns its entry), but don't
    // crash if something pulled it out of the map first.
    return;
  }

  uint32_t parentId = it->second.parentId;
  if (it->second.timeout) {
    // Already fired; just tear the timer down.
    it->second.timeout->close();
    it->second.timeout.reset();
  }
  _pendingParentClassLookups.erase(it);

  spdlog::get("ca")->warn(
      "Client: {} parent class lookup timed out for parent {} (ctx {})",
      _channel, parentId, context);
}

uint16_t ClientParticipant::AllocateInternalInterestId() {
  // Walk forward from the cursor, skipping any ids already in use. In
  // practice clients don't pick ids this high, so the first candidate is
  // almost always free. If we ever wrap all the way round we give up --
  // that means every one of the 65K interest slots is in use, which is
  // well beyond any sane deployment.
  uint16_t start = _nextInternalInterestId;
  do {
    uint16_t id = _nextInternalInterestId++;
    if (_nextInternalInterestId == 0) {
      // uint16 wrapped -- skip past the low range where clients usually
      // allocate to keep collision odds low on the next pass.
      _nextInternalInterestId = 0xF000;
    }
    if (!_interests.contains(id)) {
      return id;
    }
  } while (_nextInternalInterestId != start);

  spdlog::get("ca")->error(
      "Client: {} ran out of interest id space allocating internal slot",
      _channel);
  return 0;
}

void ClientParticipant::ApplyAvatarParentRule(const uint32_t& parentId,
                                              const ParentingRule& rule) {
  // Stated / unparsable rules: just record the parent for dedupe. No
  // internal interest is opened.
  if (rule.kind == ParentingRuleKind::Stated ||
      rule.kind == ParentingRuleKind::Auto) {
    // Auto is a per-interest-open rule, not an avatar-parent one, so for
    // the purposes of tracking the avatar's parent it behaves like Stated.
    _avatarParentRule = AvatarParentRule{parentId, rule, 0};
    return;
  }

  std::unordered_set<uint32_t> zones;
  if (rule.kind == ParentingRuleKind::Follow) {
    if (_avatarZone != INVALID_DO_ID) {
      zones.insert(_avatarZone);
    }
  } else if (rule.kind == ParentingRuleKind::Cartesian) {
    zones = CartesianGridZones(rule, _avatarZone);
  }

  uint16_t id = AllocateInternalInterestId();
  _avatarParentRule = AvatarParentRule{parentId, rule, id};

  if (zones.empty()) {
    // Off-grid or no avatar zone yet; nothing to open, but keep the slot
    // recorded so zone updates can populate it later.
    return;
  }

  Interest i;
  i.id = id;
  i.parent = parentId;
  i.zones = std::move(zones);
  i.isInternal = true;
  AddInterest(i, /*context=*/0, /*caller=*/0);

  spdlog::get("ca")->debug(
      "Client: {} opened rule-based interest {} for parent {} with {} "
      "zone(s)",
      _channel, id, parentId, i.zones.size());
}

void ClientParticipant::UpdateAvatarParentRule() {
  // Called when the avatar's zone (but not parent) changes. Only Follow
  // and Cartesian care about zone; Stated/Auto are zone-agnostic.
  if (!_avatarParentRule) {
    return;
  }
  const ParentingRule& rule = _avatarParentRule->rule;
  if (rule.kind != ParentingRuleKind::Follow &&
      rule.kind != ParentingRuleKind::Cartesian) {
    return;
  }

  std::unordered_set<uint32_t> zones;
  if (rule.kind == ParentingRuleKind::Follow) {
    if (_avatarZone != INVALID_DO_ID) {
      zones.insert(_avatarZone);
    }
  } else {
    zones = CartesianGridZones(rule, _avatarZone);
  }

  // If we've never actually opened an interest yet (e.g. initial zone was
  // off-grid), do it now rather than modifying.
  if (!_interests.contains(_avatarParentRule->interestId)) {
    if (zones.empty()) {
      return;  // still nothing to open.
    }
    Interest i;
    i.id = _avatarParentRule->interestId;
    i.parent = _avatarParentRule->parentId;
    i.zones = std::move(zones);
    i.isInternal = true;
    AddInterest(i, /*context=*/0, /*caller=*/0);
    return;
  }

  // Already-open interest: AddInterest handles the diff for us by comparing
  // the incoming zone set against the stored one in _interests.
  Interest i = _interests[_avatarParentRule->interestId];
  i.zones = std::move(zones);
  i.isInternal = true;
  AddInterest(i, /*context=*/0, /*caller=*/0);
}

void ClientParticipant::ClearAvatarParentRule() {
  if (!_avatarParentRule) {
    return;
  }

  uint16_t id = _avatarParentRule->interestId;
  auto it = _interests.find(id);
  if (it != _interests.end()) {
    Interest i = it->second;
    RemoveInterest(i, /*context=*/0, /*caller=*/0);
  }
  _avatarParentRule.reset();
}

void ClientParticipant::MaybeApplyAutoRule(Interest& i) {
  // Auto needs the parent's class to know which origin zone / extras
  // apply. The open path has to stay synchronous so we can only consult
  // parents that are already visible; a non-visible parent quietly falls
  // through to a plain (non-extended) interest.
  DCClass* parentClass = LookupObject(i.parent);
  if (!parentClass) {
    return;
  }

  ParentingRule rule;
  if (!TryParseParentingRule(parentClass, rule)) {
    return;
  }
  if (rule.kind != ParentingRuleKind::Auto) {
    return;
  }

  if (!i.zones.contains(rule.autoOriginZone)) {
    return;
  }

  for (const uint32_t& extra : rule.autoExtraZones) {
    i.zones.insert(extra);
  }

  spdlog::get("ca")->debug(
      "Client: {} applied Auto rule on parent {} (origin zone {}), "
      "merged {} extra zone(s) into interest {}",
      _channel, i.parent, rule.autoOriginZone, rule.autoExtraZones.size(),
      i.id);
}

}  // namespace Ardos
