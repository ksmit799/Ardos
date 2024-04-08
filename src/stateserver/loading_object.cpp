#include "loading_object.h"

#include "../util/logger.h"

namespace Ardos {

LoadingObject::LoadingObject(DatabaseStateServer *stateServer,
                             const uint32_t &doId, const uint32_t &parentId,
                             const uint32_t &zoneId,
                             const std::unordered_set<uint32_t> &contexts)
    : ChannelSubscriber(), _stateServer(stateServer), _doId(doId),
      _parentId(parentId), _zoneId(zoneId),
      _context(stateServer->_nextContext++), _validContexts(contexts),
      _startTime(g_loop->now()) {
  SubscribeChannel(doId);

  if (_stateServer->_loadingGauge) {
    _stateServer->_loadingGauge->Increment();
  }
}

LoadingObject::LoadingObject(DatabaseStateServer *stateServer,
                             const uint32_t &doId, const uint32_t &parentId,
                             const uint32_t &zoneId, DCClass *dclass,
                             DatagramIterator &dgi,
                             const std::unordered_set<uint32_t> &contexts)
    : ChannelSubscriber(), _stateServer(stateServer), _doId(doId),
      _parentId(parentId), _zoneId(zoneId),
      _context(stateServer->_nextContext++), _validContexts(contexts),
      _dclass(dclass), _startTime(g_loop->now()) {
  SubscribeChannel(doId);

  // Unpack the RAM fields we received in the generate message.
  auto fieldCount = dgi.GetUint16();
  for (size_t i = 0; i < fieldCount; ++i) {
    auto fieldId = dgi.GetUint16();

    auto field = _dclass->get_field_by_index(fieldId);
    if (!field) {
      Logger::Error(std::format("[DBSS] Loading object: {} received invalid "
                                "field index on generate: {}",
                                _doId, fieldId));
      return;
    }

    if (field->is_ram() || field->is_required()) {
      dgi.UnpackField(field, _fieldUpdates[field]);
    } else {
      Logger::Error(std::format(
          "[DBSS] Loading object: {} received non-RAM field on generate: {}",
          _doId, field->get_name()));
    }
  }

  if (_stateServer->_loadingGauge) {
    _stateServer->_loadingGauge->Increment();
  }
}

void LoadingObject::Start() {
  if (_validContexts.empty()) {
    // Fetch our stored fields from the database.
    auto dg = std::make_shared<Datagram>(_stateServer->_dbChannel, _doId,
                                         DBSERVER_OBJECT_GET_ALL);
    dg->AddUint32(_context);
    dg->AddUint32(_doId);
    PublishDatagram(dg);
  }
}

void LoadingObject::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Skip MD routing headers.
  dgi.SeekPayload();

  try {
    uint64_t sender = dgi.GetUint64();
    uint16_t msgType = dgi.GetUint16();
    switch (msgType) {
    case DBSERVER_OBJECT_GET_ALL_RESP: {
      if (_isLoaded) {
        break;
      }

      // Make sure the context from the database is valid.
      uint32_t context = dgi.GetUint32();
      if (context != _context && !_validContexts.contains(context)) {
        Logger::Warn(std::format("[DBSS] Loading object: {} received "
                                 "GET_ALL_RESP with invalid context: {}",
                                 _doId, context));
        break;
      }

      Logger::Verbose(std::format(
          "[DBSS] Loading object: {} received GET_ALL_RESP", _doId));
      _isLoaded = true;

      if (!dgi.GetBool()) {
        Logger::Verbose(std::format(
            "[DBSS] Loading object: {} was not found in database", _doId));
        Finalize();
        break;
      }

      uint16_t dcId = dgi.GetUint16();
      auto dcClass = g_dc_file->get_class(dcId);
      if (!dcClass) {
        Logger::Error(std::format("[DBSS] Loading object: {} received invalid "
                                  "dclass from database: {}",
                                  _doId, dcId));
        Finalize();
        break;
      }

      // Make sure both dclass's match if we were supplied with one.
      if (_dclass && dcClass != _dclass) {
        Logger::Error(std::format(
            "[DBSS] Loading object: {} received mismatched dclass: {} - {}",
            _doId, _dclass->get_name(), dcClass->get_name()));
        Finalize();
        break;
      }

      // Unpack fields from database.
      if (!UnpackDBFields(dgi, dcClass, _requiredFields, _ramFields)) {
        Logger::Error(std::format(
            "[DBSS] Loading object: {} failed to unpack fields from database.",
            _doId));
        Finalize();
        break;
      }

      // Add default values and update values.
      auto numFields = dcClass->get_num_inherited_fields();
      for (size_t i = 0; i < numFields; ++i) {
        auto field = dcClass->get_inherited_field(i);
        if (!field->as_molecular_field()) {
          if (field->is_required()) {
            if (_fieldUpdates.contains(field)) {
              _requiredFields[field] = _fieldUpdates[field];
            } else if (!_requiredFields.contains(field)) {
              _requiredFields[field] = field->get_default_value();
            }
          } else if (field->is_ram()) {
            if (_fieldUpdates.contains(field)) {
              _ramFields[field] = _fieldUpdates[field];
            }
          }
        }
      }

      // Create object on state server.
      auto distObj = new DistributedObject(
          _stateServer, _stateServer->_dbChannel, _doId, _parentId, _zoneId,
          dcClass, _requiredFields, _ramFields);

      // Tell DBSS about object and handle datagram queue.
      _stateServer->ReceiveObject(distObj);
      ReplayDatagrams(distObj);

      // Cleanup this loader.
      Finalize();
      break;
    }
    case DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS:
    case DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS_OTHER:
      // Don't cache these messages in the queue, they are received and
      // handled by the DBSS.  Since the object is already loading they
      // are simply ignored (the DBSS may generate a warning/error).
      break;
    default:
      _datagramQueue.push_back(dg);
      break;
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error(std::format(
        "[DBSS] Loading object: {} received a truncated datagram!", _doId));
  }
}

void LoadingObject::Finalize() {
  _stateServer->ReportActivateTime(_startTime);
  _stateServer->DiscardLoader(_doId);
  ForwardDatagrams();
  ChannelSubscriber::Shutdown();
}

void LoadingObject::ReplayDatagrams(DistributedObject *distObj) {
  Logger::Verbose(std::format(
      "[DBSS] Loading object: {} replaying datagrams received while loading...",
      _doId));
  for (const auto &dg : _datagramQueue) {
    if (!_stateServer->_distObjs.contains(_doId)) {
      Logger::Verbose(
          std::format("[DBSS] Deleted while replaying, aborting...", _doId));
      return;
    }

    distObj->HandleDatagram(dg);
  }

  Logger::Verbose(std::format("[DBSS] Replay finished.", _doId));
}

void LoadingObject::ForwardDatagrams() {
  Logger::Verbose(std::format("[DBSS] Loading object: {} forwarding datagrams "
                              "received while loading...",
                              _doId));
  for (const auto &dg : _datagramQueue) {
    _stateServer->HandleDatagram(dg);
  }
  Logger::Verbose(std::format("[DBSS] Finished forwarding.", _doId));
}

} // namespace Ardos