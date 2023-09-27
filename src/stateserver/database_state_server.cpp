#include "database_state_server.h"

#include <format>

#include "../util/config.h"
#include "../util/logger.h"

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
  auto min = rangeParam["min"].as<uint32_t>();
  auto max = rangeParam["max"].as<uint32_t>();

  // Start listening to DoId's in our listening range.
  SubscribeRange(min, max);
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

  if (other) {
  }
}

void DatabaseStateServer::HandleDeleteDisk(DatagramIterator &dgi,
                                           const uint64_t &sender) {}

void DatabaseStateServer::HandleSetField(DatagramIterator &dgi,
                                         const bool &multiple) {}

void DatabaseStateServer::HandleGetField(DatagramIterator &dgi,
                                         const uint64_t &sender,
                                         const bool &multiple) {}

void DatabaseStateServer::HandleGetFieldResp(DatagramIterator &dgi,
                                             const bool &multiple) {}

void DatabaseStateServer::HandleGetAll(DatagramIterator &dgi,
                                       const uint64_t &sender) {}

void DatabaseStateServer::HandleGetAllResp(DatagramIterator &dgi) {}

void DatabaseStateServer::HandleGetActivated(DatagramIterator &dgi,
                                             const uint64_t &sender) {}

} // namespace Ardos
