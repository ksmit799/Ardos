#include "database_state_server.h"

#include <format>

#include "../util/config.h"
#include "../util/logger.h"
#include "../util/metrics.h"
#include "../web/web_panel.h"
#include "loading_object.h"

namespace Ardos {

DatabaseStateServer::DatabaseStateServer() : ChannelSubscriber() {
  Logger::Info("Starting Database State Server component...");

  // Database State Server configuration.
  auto config = Config::Instance()->GetNode("db-state-server");
  if (!config["database"]) {
    Logger::Error("[DBSS] Missing or invalid database channel!");
    exit(1);
  }

  // Start listening to our broadcast channel.
  SubscribeChannel(BCHAN_STATESERVERS);

  // Database channel.
  _dbChannel = config["database"].as<uint64_t>();

  auto rangeParam = config["ranges"];
  _minDoId = rangeParam["min"].as<uint64_t>();
  _maxDoId = rangeParam["max"].as<uint64_t>();

  // Start listening to DoId's in our listening range.
  SubscribeRange(_minDoId, _maxDoId);

  // Initialize metrics.
  InitMetrics();
}

void DatabaseStateServer::ReceiveObject(DistributedObject *distObj) {
  _distObjs[distObj->GetDoId()] = distObj;

  if (_objectsGauge) {
    _objectsGauge->Increment();
  }

  if (_objectsSize) {
    _objectsSize->Observe((double)distObj->Size());
  }
}

void DatabaseStateServer::RemoveDistributedObject(const uint32_t &doId) {
  _distObjs.erase(doId);

  if (_objectsGauge) {
    _objectsGauge->Decrement();
  }
}

void DatabaseStateServer::DiscardLoader(const uint32_t &doId) {
  _loadObjs.erase(doId);

  if (_loadingGauge) {
    _loadingGauge->Decrement();
  }
}

void DatabaseStateServer::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Skip MD routing headers.
  dgi.SeekPayload();

  try {
    uint64_t sender = dgi.GetUint64();
    uint16_t msgType = dgi.GetUint16();
    switch (msgType) {
    case DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS:
      HandleActivate(dgi, false);
      break;
    case DBSS_OBJECT_ACTIVATE_WITH_DEFAULTS_OTHER:
      HandleActivate(dgi, true);
      break;
    case DBSS_OBJECT_DELETE_DISK:
      HandleDeleteDisk(dgi, sender);
      break;
    case STATESERVER_OBJECT_SET_FIELD:
    case STATESERVER_OBJECT_SET_FIELDS:
      HandleSetField(dgi, msgType == STATESERVER_OBJECT_SET_FIELDS);
      break;
    case STATESERVER_OBJECT_GET_FIELD:
    case STATESERVER_OBJECT_GET_FIELDS:
      HandleGetField(dgi, sender, msgType == STATESERVER_OBJECT_GET_FIELDS);
      break;
    case DBSERVER_OBJECT_GET_FIELD_RESP:
    case DBSERVER_OBJECT_GET_FIELDS_RESP:
      HandleGetFieldResp(dgi, msgType == DBSERVER_OBJECT_GET_FIELDS_RESP);
      break;
    case STATESERVER_OBJECT_GET_ALL:
      HandleGetAll(dgi, sender);
      break;
    case DBSERVER_OBJECT_GET_ALL_RESP:
      HandleGetAllResp(dgi);
      break;
    case DBSS_OBJECT_GET_ACTIVATED:
      HandleGetActivated(dgi, sender);
      break;
    default:
      // Hopefully we managed to unpack the sender...
      Logger::Verbose(std::format("[DBSS] Ignoring message: {} from sender: {}",
                                  msgType, sender));
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error("[DBSS] Received a truncated datagram!");
  }
}

void DatabaseStateServer::HandleActivate(DatagramIterator &dgi,
                                         const bool &other) {
  uint32_t doId = dgi.GetUint32();
  uint32_t parentId = dgi.GetUint32();
  uint32_t zoneId = dgi.GetUint32();

  // Make sure we don't have a duplicate generate.
  if (_distObjs.contains(doId) || _loadObjs.contains(doId)) {
    Logger::Error(
        std::format("[DBSS] Received duplicate generate for DoId: {}", doId));
    return;
  }

  // We're loading without any additional fields set.
  if (!other) {
    if (!_inactiveLoads.contains(doId)) {
      _loadObjs[doId] = new LoadingObject(this, doId, parentId, zoneId);
      _loadObjs[doId]->Start();
    } else {
      _loadObjs[doId] =
          new LoadingObject(this, doId, parentId, zoneId, _inactiveLoads[doId]);
    }
    return;
  }

  // We have some additional fields provided with our activate.
  uint16_t dcId = dgi.GetUint16();

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class(dcId);
  if (!dcClass) {
    Logger::Error(std::format(
        "[DBSS] Received ACTIVATE_OTHER with unknown distributed class {}: {}",
        doId, dcId));
    return;
  }

  if (!_inactiveLoads.contains(doId)) {
    _loadObjs[doId] =
        new LoadingObject(this, doId, parentId, zoneId, dcClass, dgi);
    _loadObjs[doId]->Start();
  } else {
    _loadObjs[doId] = new LoadingObject(this, doId, parentId, zoneId, dcClass,
                                        dgi, _inactiveLoads[doId]);
  }
}

void DatabaseStateServer::HandleDeleteDisk(DatagramIterator &dgi,
                                           const uint64_t &sender) {
  auto doId = dgi.GetUint32();
  if (_loadObjs.contains(doId)) {
    // Ignore this message for now, it'll be bounced back to us
    // from the loading object if it succeeds or fails at loading.
    return;
  }

  // If the object is loaded in memory, broadcast a delete message.
  if (_distObjs.contains(doId)) {
    auto distObj = _distObjs[doId];
    std::unordered_set<uint64_t> targets;

    // Add location to broadcast.
    if (distObj->GetLocation()) {
      targets.insert(distObj->GetLocation());
    }

    // Add AI broadcast.
    if (distObj->GetAI()) {
      targets.insert(distObj->GetAI());
    }

    // Add owner to broadcast.
    if (distObj->GetOwner()) {
      targets.insert(distObj->GetOwner());
    }

    // Send the datagram!
    auto dg =
        std::make_shared<Datagram>(targets, sender, DBSS_OBJECT_DELETE_DISK);
    dg->AddUint32(doId);
    PublishDatagram(dg);
  }

  // Send delete message to the database.
  auto dg =
      std::make_shared<Datagram>(_dbChannel, doId, DBSERVER_OBJECT_DELETE);
  dg->AddUint32(doId);
  PublishDatagram(dg);
}

void DatabaseStateServer::HandleSetField(DatagramIterator &dgi,
                                         const bool &multiple) {
  auto doId = dgi.GetUint32();
  if (_loadObjs.contains(doId)) {
    // Ignore this message for now, it'll be bounced back to us
    // from the loading object if it succeeds or fails at loading.
    return;
  }

  auto fieldCount = multiple ? dgi.GetUint16() : 1;

  auto responseType =
      multiple ? DBSERVER_OBJECT_SET_FIELDS : DBSERVER_OBJECT_SET_FIELD;

  FieldMap objectFields;
  for (size_t i = 0; i < fieldCount; ++i) {
    auto fieldId = dgi.GetUint16();

    auto field = g_dc_file->get_field_by_index(fieldId);
    if (!field) {
      Logger::Warn(std::format("[DBSS] Distributed object: {} received set "
                               "field(s) with invalid field id: {}",
                               doId, fieldId));
      continue;
    }

    if (field->is_db()) {
      dgi.UnpackField(field, objectFields[field]);
    } else {
      dgi.SkipField(field);
    }
  }

  // We didn't unpack any fields for the database.
  if (objectFields.empty()) {
    return;
  }

  auto dg = std::make_shared<Datagram>(_dbChannel, doId, responseType);
  dg->AddUint32(doId);
  if (multiple) {
    dg->AddUint16(objectFields.size());
  }
  for (const auto &it : objectFields) {
    dg->AddUint16(it.first->get_number());
    dg->AddData(it.second);
  }
  PublishDatagram(dg);
}

void DatabaseStateServer::HandleGetField(DatagramIterator &dgi,
                                         const uint64_t &sender,
                                         const bool &multiple) {
  auto ctx = dgi.GetUint32();
  auto doId = dgi.GetUint32();

  if (_distObjs.contains(doId) || _loadObjs.contains(doId)) {
    return;
  }

  auto fieldCount = multiple ? dgi.GetUint16() : 1;

  auto responseType = multiple ? STATESERVER_OBJECT_GET_FIELDS_RESP
                               : STATESERVER_OBJECT_GET_FIELD_RESP;

  std::vector<DCField *> dbFields;
  std::vector<DCField *> ramFields;
  for (size_t i = 0; i < fieldCount; ++i) {
    auto fieldId = dgi.GetUint16();

    auto field = g_dc_file->get_field_by_index(fieldId);
    if (!field) {
      auto dg = std::make_shared<Datagram>(sender, doId, responseType);
      dg->AddUint32(ctx);
      dg->AddBool(false);
      PublishDatagram(dg);
      return;
    }

    if (field->is_required() || field->is_ram()) {
      if (field->is_db()) {
        dbFields.push_back(field);
      } else {
        ramFields.push_back(field);
      }
    }
  }

  if (!dbFields.empty()) {
    // Get a new database context.
    auto dbCtx = _nextContext++;

    // Prepare response datagram.
    auto dg = std::make_shared<Datagram>(sender, doId, responseType);
    dg->AddUint32(ctx);
    dg->AddBool(true);
    if (multiple) {
      dg->AddUint16(ramFields.size() + dbFields.size());
    }
    for (const auto &field : ramFields) {
      dg->AddUint16(field->get_number());
      dg->AddData(field->get_default_value());
    }

    _contextDatagrams[dbCtx] = dg;

    // Send query off to the database.
    auto dbDg = std::make_shared<Datagram>(
        _dbChannel, doId,
        multiple ? DBSERVER_OBJECT_GET_FIELDS : DBSERVER_OBJECT_GET_FIELD);
    dbDg->AddUint32(dbCtx);
    dbDg->AddUint32(doId);
    if (multiple) {
      dbDg->AddUint16(dbFields.size());
    }
    for (const auto &field : dbFields) {
      dg->AddUint16(field->get_number());
    }
    PublishDatagram(dbDg);
  } else if (!ramFields.empty() && ramFields.back()->has_default_value()) {
    // If no database fields exist, and we have a RAM field with a default
    // value...
    auto dg = std::make_shared<Datagram>(sender, doId, responseType);
    dg->AddUint32(ctx);
    dg->AddBool(true);
    if (multiple) {
      dg->AddUint16(ramFields.size());
    }
    for (const auto &field : ramFields) {
      dg->AddUint16(field->get_number());
      dg->AddData(field->get_default_value());
    }
    PublishDatagram(dg);
  } else {
    // Otherwise, return false.
    auto dg = std::make_shared<Datagram>(sender, doId, responseType);
    dg->AddUint32(ctx);
    dg->AddBool(false);
    PublishDatagram(dg);
  }
}

void DatabaseStateServer::HandleGetFieldResp(DatagramIterator &dgi,
                                             const bool &multiple) {}

void DatabaseStateServer::HandleGetAll(DatagramIterator &dgi,
                                       const uint64_t &sender) {}

void DatabaseStateServer::HandleGetAllResp(DatagramIterator &dgi) {}

void DatabaseStateServer::HandleGetActivated(DatagramIterator &dgi,
                                             const uint64_t &sender) {
  auto ctx = dgi.GetUint32();
  auto doId = dgi.GetUint32();

  // An object is considered active if it's in memory as a distributed object.
  // If it doesn't exist, or is loading, return false.
  auto dg =
      std::make_shared<Datagram>(sender, doId, DBSS_OBJECT_GET_ACTIVATED_RESP);
  dg->AddUint32(ctx);
  dg->AddUint32(doId);
  dg->AddBool(_distObjs.contains(doId));
  PublishDatagram(dg);
}

void DatabaseStateServer::InitMetrics() {
  // Make sure we want to collect metrics on this cluster.
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  auto &objectsBuilder = prometheus::BuildGauge()
                             .Name("dbss_objects_size")
                             .Help("Number of loaded distributed objects")
                             .Register(*registry);

  auto &loadingBuilder = prometheus::BuildGauge()
                             .Name("dbss_loading_size")
                             .Help("Number of objects currently loading")
                             .Register(*registry);

  auto &activateTimeBuilder =
      prometheus::BuildHistogram()
          .Name("dbss_activate_time")
          .Help("Time taken for an object to load/activate")
          .Register(*registry);

  auto &objectsSizeBuilder =
      prometheus::BuildHistogram()
          .Name("dbss_objects_bytes_size")
          .Help("Byte-size of loaded distributed objects")
          .Register(*registry);

  _objectsGauge = &objectsBuilder.Add({});
  _loadingGauge = &loadingBuilder.Add({});

  _activateTime = &activateTimeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{
              0, 500, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000});
  _objectsSize = &objectsSizeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{0, 4, 16, 64, 256, 1024, 4096,
                                                  16384, 65536});
}

void DatabaseStateServer::ReportActivateTime(
    const uvw::timer_handle::time &startTime) {
  if (_activateTime) {
    _activateTime->Observe((double)(g_loop->now() - startTime).count());
  }
}

bool UnpackDBFields(DatagramIterator &dgi, DCClass *dclass, FieldMap &required,
                    FieldMap &ram) {
  // Unload RAM and REQUIRED fields from database response.
  auto fieldCount = dgi.GetUint16();
  for (size_t i = 0; i < fieldCount; ++i) {
    auto fieldId = dgi.GetUint16();

    auto field = dclass->get_field_by_index(fieldId);
    if (!field) {
      return false;
    }

    if (field->is_required()) {
      dgi.UnpackField(field, required[field]);
    } else if (field->is_ram()) {
      dgi.UnpackField(field, ram[field]);
    } else {
      dgi.SkipField(field);
    }
  }

  return true;
}

void DatabaseStateServer::HandleWeb(ws28::Client *client,
                                    nlohmann::json &data) {
  if (data["msg"] == "init") {
    // Build up an array of distributed objects.
    nlohmann::json distObjInfo = nlohmann::json::array();
    for (const auto &distObj : _distObjs) {
      distObjInfo.push_back({
          {"doId", distObj.first},
          {"clsName", distObj.second->GetDClass()->get_name()},
          {"parentId", distObj.second->GetParentId()},
          {"zoneId", distObj.second->GetZoneId()},
      });
    }

    WebPanel::Send(client, {
                               {"type", "dbss:init"},
                               {"success", true},
                               {"dbChannel", _dbChannel},
                               {"minDoId", _minDoId},
                               {"maxDoId", _maxDoId},
                               {"distObjs", distObjInfo},
                           });
  } else if (data["msg"] == "distobj") {
    auto doId = data["doId"].template get<uint32_t>();

    // Try to find a matching Distributed Object for the provided DoId.
    if (!_distObjs.contains(doId)) {
      WebPanel::Send(client, {
                                 {"type", "dbss:distobj"},
                                 {"success", false},
                             });
      return;
    }

    auto distObj = _distObjs[doId];

    // Build an array of explicitly set RAM fields.
    nlohmann::json ramFields = nlohmann::json::array();
    for (const auto &field : distObj->GetRamFields()) {
      ramFields.push_back({{"fieldName", field.first->get_name()}});
    }

    // Build a dictionary of zone objects under this Distributed Object.
    nlohmann::json zoneObjs = nlohmann::json::object();
    for (const auto &zoneData : distObj->GetZoneObjects()) {
      zoneObjs[zoneData.first] = zoneData.second;
    }

    WebPanel::Send(client, {
                               {"type", "dbss:distobj"},
                               {"success", true},
                               {"clsName", distObj->GetDClass()->get_name()},
                               {"parentId", distObj->GetParentId()},
                               {"zoneId", distObj->GetZoneId()},
                               {"owner", distObj->GetOwner()},
                               {"size", distObj->Size()},
                               {"ram", ramFields},
                               {"zones", zoneObjs},
                           });
  }
}

} // namespace Ardos
