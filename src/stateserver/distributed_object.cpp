#include "distributed_object.h"

#include <format>
#include <unordered_set>

#include <dcAtomicField.h>
#include <dcMolecularField.h>

#include "../util/logger.h"

namespace Ardos {

DistributedObject::DistributedObject(StateServer *stateServer,
                                     const uint32_t &doId,
                                     const uint32_t &parentId,
                                     const uint32_t &zoneId, DCClass *dclass,
                                     DatagramIterator &dgi, const bool &other)
    : ChannelSubscriber(), _stateServer(stateServer), _doId(doId),
      _parentId(parentId), _zoneId(zoneId), _dclass(dclass) {
  // Unpack required fields.
  for (int i = 0; i < _dclass->get_num_inherited_fields(); ++i) {
    auto field = _dclass->get_inherited_field(i);
    if (field->is_required() && !field->as_molecular_field()) {
      dgi.UnpackField(field, _requiredFields[field]);
    }
  }

  // Unpack extra fields if supplied.
  if (other) {
    uint16_t count = dgi.GetUint16();
    for (int i = 0; i < count; ++i) {
      uint16_t fieldId = dgi.GetUint16();
      auto field = _dclass->get_field_by_index(fieldId);
      if (!field) {
        Logger::Error(std::format(
            "[SS] Received generated with unknown field id: {} for DoId: {}",
            fieldId, _doId));
        break;
      }

      // We only handle 'RAM' fields. If they're not to be stored on the SS,
      // then that's an error.
      if (field->is_ram()) {
        dgi.UnpackField(field, _ramFields[field]);
      } else {
        Logger::Error(std::format(
            "[SS] Received generated with non RAM field: {} for DoId: ",
            field->get_name(), _doId));
      }
    }
  }

  SubscribeChannel(_doId);

  Logger::Verbose(
      std::format("[SS] Distributed Object: '{}' generated with DoId: {}",
                  _dclass->get_name(), _doId));

  dgi.SeekPayload();
  HandleLocationChange(parentId, zoneId, dgi.GetUint64());
  WakeChildren();
}

uint64_t DistributedObject::GetAI() const { return _aiChannel; }

bool DistributedObject::IsAIExplicitlySet() const { return _aiExplicitlySet; }

void DistributedObject::Annihilate(const uint64_t &sender,
                                   const bool &notifyParent) {
  std::unordered_set<uint64_t> targets;

  if (_parentId) {
    targets.insert(LocationAsChannel(_parentId, _zoneId));
    if (notifyParent) {
      auto dg = std::make_shared<Datagram>(
          _parentId, sender, STATESERVER_OBJECT_CHANGING_LOCATION);
      dg->AddUint32(_doId);
      dg->AddLocation(INVALID_DO_ID, INVALID_DO_ID);
      dg->AddLocation(_parentId, _zoneId);
      PublishDatagram(dg);
    }
  }

  if (_ownerChannel) {
    targets.insert(_ownerChannel);
  }

  if (_aiChannel) {
    targets.insert(_aiChannel);
  }

  auto dg = std::make_shared<Datagram>(targets, sender,
                                       STATESERVER_OBJECT_DELETE_RAM);
  dg->AddUint32(_doId);
  PublishDatagram(dg);

  DeleteChildren(sender);

  _stateServer->RemoveDistributedObject(_doId);
  Logger::Verbose(std::format("[SS] Distributed Object: '{}' deleted.", _doId));

  Shutdown();
}

void DistributedObject::DeleteChildren(const uint64_t &sender) {
  if (!_zoneObjects.empty()) {
    // We have at least one child, notify them.
    auto dg = std::make_shared<Datagram>(ParentToChildren(_doId), sender,
                                         STATESERVER_OBJECT_DELETE_CHILDREN);
    dg->AddUint32(_doId);
    PublishDatagram(dg);
  }
}

void DistributedObject::HandleDatagram(const std::shared_ptr<Datagram> &dgIn) {
  DatagramIterator dgi(dgIn);

  // Skip MD routing headers.
  dgi.SeekPayload();

  uint64_t sender = dgi.GetUint64();
  uint16_t msgType = dgi.GetUint16();
  switch (msgType) {
  case STATESERVER_DELETE_AI_OBJECTS: {
    uint64_t channel = dgi.GetUint64();
    if (_aiChannel != channel) {
      Logger::Warn(std::format("[SS] Distributed Object: '{}' ({}) received "
                               "delete for wrong AI channel: {}",
                               _doId, _aiChannel, channel));
      break;
    }
    Annihilate(sender);
    break;
  }
  case STATESERVER_OBJECT_DELETE_RAM: {
    if (_doId != dgi.GetUint32()) {
      break;
    }
    Annihilate(sender);
    break;
  }
  case STATESERVER_OBJECT_DELETE_CHILDREN: {
    uint32_t targetDoId = dgi.GetUint32();
    if (targetDoId == _doId) {
      DeleteChildren(sender);
    } else if (targetDoId == _parentId) {
      Annihilate(sender, false);
    }
    break;
  }
  case STATESERVER_OBJECT_SET_FIELD: {
    if (_doId != dgi.GetUint32()) {
      break;
    }
    HandleOneUpdate(dgi, sender);
    break;
  }
  case STATESERVER_OBJECT_SET_FIELDS: {
    if (_doId != dgi.GetUint32()) {
      break;
    }
    uint16_t fieldCount = dgi.GetUint16();
    for (uint16_t i = 0; i < fieldCount; ++i) {
      if (!HandleOneUpdate(dgi, sender)) {
        break;
      }
    }
    break;
  }
  case STATESERVER_OBJECT_CHANGING_AI: {
    uint32_t parentId = dgi.GetUint32();
    uint64_t newChannel = dgi.GetUint64();

    Logger::Verbose(std::format(
        "[SS] Distributed Object: '{}' received changing AI from: {}", _doId,
        parentId));

    if (parentId != _parentId) {
      Logger::Warn(std::format("[SS] Distributed Object: '{}' received "
                               "changing AI from: {} but my parent is: {}",
                               _doId, parentId, _parentId));
      break;
    }

    if (_aiExplicitlySet) {
      break;
    }

    HandleAIChange(newChannel, sender, false);
    break;
  }
  case STATESERVER_OBJECT_SET_AI: {
    uint64_t newChannel = dgi.GetUint64();

    Logger::Verbose(std::format(
        "[SS] Distributed Object: '{}' updating AI to: {}", _doId, newChannel));

    HandleAIChange(newChannel, sender, true);
    break;
  }
  case STATESERVER_OBJECT_GET_AI: {
    Logger::Verbose(std::format(
        "[SS] Distributed Object: '{}' received AI query from: ", _doId,
        sender));

    auto dg = std::make_shared<Datagram>(sender, _doId,
                                         STATESERVER_OBJECT_GET_AI_RESP);
    dg->AddUint32(dgi.GetUint32()); // Get context.
    dg->AddUint32(_doId);
    dg->AddUint64(_aiChannel);
    PublishDatagram(dg);
    break;
  }
  case STATESERVER_OBJECT_GET_AI_RESP: {
    dgi.GetUint32(); // Discard context.
    uint32_t parentId = dgi.GetUint32();

    Logger::Verbose(std::format(
        "[SS] Distributed Object: '{}' received AI query response from: {}",
        _doId, parentId));

    if (parentId != _parentId) {
      Logger::Warn(std::format("[SS] Distributed Object: '{}' received AI "
                               "channel from: {} but my parent is: {}",
                               _doId, parentId, _parentId));
      break;
    }

    uint64_t newAI = dgi.GetUint64();
    if (_aiExplicitlySet) {
      break;
    }

    HandleAIChange(newAI, sender, false);
    break;
  }
  case STATESERVER_OBJECT_CHANGING_LOCATION: {
    break;
  }
  case STATESERVER_OBJECT_LOCATION_ACK: {
    break;
  }
  case STATESERVER_OBJECT_SET_LOCATION: {
    uint32_t newParent = dgi.GetUint32();
    uint32_t newZone = dgi.GetUint32();

    Logger::Verbose(
        std::format("[SS] Distributed Object: '{}' updating location to: {}/{}",
                    _doId, newParent, newZone));

    HandleLocationChange(newParent, newZone, sender);
    break;
  }
  case STATESERVER_OBJECT_GET_LOCATION: {
    uint32_t context = dgi.GetUint32();

    auto dg = std::make_shared<Datagram>(sender, _doId,
                                         STATESERVER_OBJECT_GET_LOCATION_RESP);
    dg->AddUint32(context);
    dg->AddUint32(_doId);
    dg->AddLocation(_parentId, _zoneId);
    PublishDatagram(dg);
    break;
  }
  case STATESERVER_OBJECT_GET_LOCATION_RESP: {
    // This case occurs immediately after object creation.
    // A parent expects to receive a location_resp from each
    // of its pre-existing children.

    if (dgi.GetUint32() != STATESERVER_CONTEXT_WAKE_CHILDREN) {
      Logger::Warn(std::format("[SS] Distributed Object: '{}' received "
                               "unexpected location response from: {}",
                               _doId, dgi.GetUint32()));
      break;
    }

    // The DoId of our child.
    uint32_t doId = dgi.GetUint32();

    // The location of our child.
    uint32_t parentId = dgi.GetUint32();
    uint32_t zoneId = dgi.GetUint32();

    // Insert the child DoId into the specified zone.
    if (parentId == _parentId) {
      _zoneObjects[zoneId].insert(doId);
    }
    break;
  }
  case STATESERVER_OBJECT_GET_ALL: {
    uint32_t context = dgi.GetUint32();
    if (dgi.GetUint32() != _doId) {
      break;
    }

    auto dg = std::make_shared<Datagram>(sender, _doId,
                                         STATESERVER_OBJECT_GET_ALL_RESP);
    dg->AddUint32(context);
    AppendRequiredData(dg);
    if (!_ramFields.empty()) {
      AppendOtherData(dg);
    }
    PublishDatagram(dg);
    break;
  }
  case STATESERVER_OBJECT_GET_FIELD: {
    break;
  }
  case STATESERVER_OBJECT_GET_FIELDS: {
    break;
  }
  case STATESERVER_OBJECT_SET_OWNER: {
    break;
  }
  case STATESERVER_OBJECT_GET_ZONE_OBJECTS:
  case STATESERVER_OBJECT_GET_ZONES_OBJECTS: {
    break;
  }
  case STATESERVER_GET_ACTIVE_ZONES: {
    break;
  }
  default:
    Logger::Warn(std::format(
        "[SS] Distributed Object: '{}' ignoring unknown message type: ", _doId,
        msgType));
  }
}

void DistributedObject::HandleLocationChange(const uint32_t &newParent,
                                             const uint32_t &newZone,
                                             const uint64_t &sender) {
  uint32_t oldParent = _parentId;
  uint32_t oldZone = _zoneId;

  // Set of channels that must be notified about our location change.
  std::unordered_set<uint64_t> targets;

  // Notify AI of our changing location.
  if (_aiChannel) {
    targets.insert(_aiChannel);
  }

  // Notify owner of our changing location.
  if (_ownerChannel) {
    targets.insert(_ownerChannel);
  }

  // Make sure we're not breaking our DO tree.
  if (newParent == _doId) {
    Logger::Warn(std::format(
        "[SS] Distributed Object: '{}' cannot be parented to itself.", _doId));
    return;
  }

  // Handle parent change.
  if (newParent != oldParent) {
    // Unsubscribe from the old parent's child-broadcast channel.
    if (oldParent) {
      UnsubscribeChannel(ParentToChildren(oldParent));

      // Notify the old parent and location of changing location.
      targets.insert(oldParent);
      targets.insert(LocationAsChannel(oldParent, oldZone));
    }

    _parentId = newParent;
    _zoneId = newZone;

    // Subscribe to the new parent's child-broadcast channel.
    if (newParent) {
      SubscribeChannel(ParentToChildren(_parentId));

      if (!_aiExplicitlySet) {
        // Ask the new parent what it's managing AI is.
        auto dg = std::make_shared<Datagram>(_parentId, _doId,
                                             STATESERVER_OBJECT_GET_AI);
        dg->AddUint32(_nextContext++);
        PublishDatagram(dg);
      }

      // Notify our new parent of our changing location.
      targets.insert(newParent);
    } else if (!_aiExplicitlySet) {
      _aiChannel = INVALID_CHANNEL;
    }
  } else if (newZone != oldZone) {
    _zoneId = newZone;
    // Notify our parent and old location of our changing location.
    targets.insert(_parentId);
    targets.insert(LocationAsChannel(_parentId, oldZone));
  } else {
    // We're not actually changing location, no need to handle.
    return;
  }

  // Send changing location message.
  auto dg = std::make_shared<Datagram>(targets, sender,
                                       STATESERVER_OBJECT_CHANGING_LOCATION);
  dg->AddUint32(_doId);
  dg->AddLocation(newParent, newZone);
  dg->AddLocation(oldParent, oldZone);
  PublishDatagram(dg);

  // At this point the new parent (which may or may not be the same as the old
  // parent) is unaware of our existence in this zone.
  _parentSynchronized = false;

  // Send enter location message.
  if (newParent) {
    SendLocationEntry(LocationAsChannel(newParent, newZone));
  }
}

void DistributedObject::HandleAIChange(const uint64_t &newAI,
                                       const uint64_t &sender,
                                       const bool &channelIsExplicit) {
  uint64_t oldAI = _aiChannel;
  if (newAI == oldAI) {
    return;
  }

  // Set of channels that must be notified of our AI change.
  std::unordered_set<uint64_t> targets;

  if (oldAI) {
    targets.insert(oldAI);
  }

  if (!_zoneObjects.empty()) {
    // Notify our children as well.
    targets.insert(ParentToChildren(_doId));
  }

  _aiChannel = newAI;
  _aiExplicitlySet = channelIsExplicit;

  auto dg = std::make_shared<Datagram>(targets, sender,
                                       STATESERVER_OBJECT_CHANGING_AI);
  dg->AddUint32(_doId);
  dg->AddUint64(newAI);
  dg->AddUint64(oldAI);
  PublishDatagram(dg);

  if (newAI) {
    Logger::Verbose(std::format(
        "[SS] Distributed Object: '{}' sending AI entry to: ", _doId, newAI));
    SendAIEntry(newAI);
  }
}

void DistributedObject::WakeChildren() {
  auto dg = std::make_shared<Datagram>(ParentToChildren(_doId), _doId,
                                       STATESERVER_OBJECT_GET_LOCATION);
  dg->AddUint32(STATESERVER_CONTEXT_WAKE_CHILDREN);
  PublishDatagram(dg);
}

void DistributedObject::SendLocationEntry(const uint64_t &location) {
  auto dg = std::make_shared<Datagram>(
      location, _doId,
      _ramFields.empty()
          ? STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED
          : STATESERVER_OBJECT_ENTER_LOCATION_WITH_REQUIRED_OTHER);

  AppendRequiredData(dg, true);
  if (!_ramFields.empty()) {
    AppendOtherData(dg, true);
  }

  PublishDatagram(dg);
}

void DistributedObject::SendAIEntry(const uint64_t &location) {
  auto dg = std::make_shared<Datagram>(
      location, _doId,
      _ramFields.empty() ? STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED
                         : STATESERVER_OBJECT_ENTER_AI_WITH_REQUIRED_OTHER);

  AppendRequiredData(dg);
  if (!_ramFields.empty()) {
    AppendOtherData(dg);
  }

  PublishDatagram(dg);
}

void DistributedObject::AppendRequiredData(const std::shared_ptr<Datagram> &dg,
                                           const bool &clientOnly,
                                           const bool &alsoOwner) {
  dg->AddUint32(_doId);
  dg->AddLocation(_parentId, _zoneId);
  dg->AddUint16(_dclass->get_number());

  size_t fieldCount = _dclass->get_num_inherited_fields();
  for (int i = 0; i < fieldCount; ++i) {
    DCField *field = _dclass->get_inherited_field(i);
    if (field->is_required() && !field->as_molecular_field() &&
        (!clientOnly || field->is_broadcast() || field->is_clrecv() ||
         (alsoOwner && field->is_ownrecv()))) {
      dg->AddData(_requiredFields[field]);
    }
  }
}

void DistributedObject::AppendOtherData(const std::shared_ptr<Datagram> &dg,
                                        const bool &clientOnly,
                                        const bool &alsoOwner) {
  if (clientOnly) {
    std::vector<const DCField *> broadcastFields;
    for (const auto &field : _ramFields) {
      if (field.first->is_broadcast() || field.first->is_clrecv() ||
          (alsoOwner && field.first->is_ownrecv())) {
        broadcastFields.push_back(field.first);
      }
    }

    dg->AddUint16(broadcastFields.size());
    for (const auto &field : broadcastFields) {
      dg->AddUint16(field->get_number());
      dg->AddData(_ramFields[field]);
    }
  } else {
    dg->AddUint16(_ramFields.size());
    for (const auto &field : _ramFields) {
      dg->AddUint16(field.first->get_number());
      dg->AddData(field.second);
    }
  }
}

void DistributedObject::SaveField(DCField *field,
                                  const std::vector<uint8_t> &data) {
  if (field->is_required()) {
    _requiredFields[field] = data;
  } else if (field->is_ram()) {
    _ramFields[field] = data;
  }
}

bool DistributedObject::HandleOneUpdate(DatagramIterator &dgi,
                                        const uint64_t &sender) {
  std::vector<uint8_t> data;
  uint16_t fieldId = dgi.GetUint16();

  DCField *field = _dclass->get_inherited_field(fieldId);
  if (!field) {
    Logger::Error(std::format("[SS] Distributed Object: '{}' received field "
                              "update for invalid field: {} - {}",
                              _doId, fieldId, _dclass->get_name()));
    return false;
  }

  Logger::Verbose(
      std::format("[SS] Distributed Object: '{}' handling field update for: {}",
                  _doId, field->get_name()));

  uint16_t fieldStart = dgi.Tell();

  try {
    dgi.UnpackField(field, data);
  } catch (const DatagramIteratorEOF &) {
    Logger::Error(std::format(
        "[SS] Distributed Object: '{}' received truncated field update for: {}",
        _doId, field->get_name()));
    return false;
  }

  DCMolecularField *molecular = field->as_molecular_field();
  if (molecular) {
    dgi.Seek(fieldStart);
    int n = molecular->get_num_atomics();
    for (int i = 0; i < n; ++i) {
      std::vector<uint8_t> fieldData;
      DCAtomicField *atomic = molecular->get_atomic(i);
      dgi.UnpackField(atomic, fieldData);
      SaveField(atomic, fieldData);
    }
  } else {
    SaveField(field, data);
  }

  std::unordered_set<uint64_t> targets;

  if (field->is_broadcast()) {
    targets.insert(LocationAsChannel(_parentId, _zoneId));
  }

  if (field->is_airecv() && _aiChannel && _aiChannel != sender) {
    targets.insert(_aiChannel);
  }

  if (field->is_ownrecv() && _ownerChannel && _ownerChannel != sender) {
    targets.insert(_ownerChannel);
  }

  auto dg =
      std::make_shared<Datagram>(targets, sender, STATESERVER_OBJECT_SET_FIELD);
  dg->AddUint32(_doId);
  dg->AddUint16(fieldId);
  dg->AddData(data);
  PublishDatagram(dg);

  return true;
}

} // namespace Ardos
