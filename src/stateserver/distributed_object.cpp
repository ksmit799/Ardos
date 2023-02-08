#include "distributed_object.h"

#include <format>
#include <unordered_set>

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

      // We only handle 'RAM' fields, if they're not to be stored on the SS then
      // that's an error.
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

void DistributedObject::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Skip MD routing headers.
  dgi.SeekPayload();

  uint64_t sender = dgi.GetUint64();
  uint16_t msgType = dgi.GetUint16();
  switch (msgType) {
  case STATESERVER_DELETE_AI_OBJECTS: {
    break;
  }
  case STATESERVER_OBJECT_DELETE_RAM: {
    break;
  }
  case STATESERVER_OBJECT_DELETE_CHILDREN: {
    break;
  }
  case STATESERVER_OBJECT_SET_FIELD: {
    break;
  }
  case STATESERVER_OBJECT_SET_FIELDS: {
    break;
  }
  case STATESERVER_OBJECT_CHANGING_AI: {
    break;
  }
  case STATESERVER_OBJECT_SET_AI: {
    break;
  }
  case STATESERVER_OBJECT_GET_AI: {
    break;
  }
  case STATESERVER_OBJECT_GET_AI_RESP: {
    break;
  }
  case STATESERVER_OBJECT_CHANGING_LOCATION: {
    break;
  }
  case STATESERVER_OBJECT_LOCATION_ACK: {
    break;
  }
  case STATESERVER_OBJECT_SET_LOCATION: {
    break;
  }
  case STATESERVER_OBJECT_GET_LOCATION: {
    break;
  }
  case STATESERVER_OBJECT_GET_LOCATION_RESP: {
    break;
  }
  case STATESERVER_OBJECT_GET_ALL: {
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

} // namespace Ardos
