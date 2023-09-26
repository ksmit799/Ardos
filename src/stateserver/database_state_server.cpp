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

} // namespace Ardos
