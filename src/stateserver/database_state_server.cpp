#include "database_state_server.h"

#include <format>

#include "../util/config.h"
#include "../util/logger.h"

namespace Ardos {

DatabaseStateServer::DatabaseStateServer() : ChannelSubscriber() {
  Logger::Info("Starting Database State Server component...");

  // Database State Server configuration.
  auto config = Config::Instance()->GetNode("db-state-server");
  if (!config["channel"]) {
    Logger::Error("[DBSS] Missing or invalid channel!");
    exit(1);
  }

  // Start listening to our channel.
  _channel = config["channel"].as<uint64_t>();
  SubscribeChannel(_channel);
  SubscribeChannel(BCHAN_STATESERVERS);

  // Database channel.
  _dbChannel = config["database"].as<uint64_t>();

  auto rangeParam = config["range"];
  auto min = rangeParam["min"].as<uint32_t>();
  auto max = rangeParam["max"].as<uint32_t>();

  // Start listening to DoId's in our listening range.
  SubscribeRange(min, max);
}

/**
 * Subscribe to the DoId's of all database objects within our range.
 * TODO: Evaluate the performance of this. We cannot subscribe to a *range* of
 * channels exactly due to RabbitMQ wildcard limitations.
 */
void DatabaseStateServer::SubscribeRange(const uint32_t &min, const uint32_t &max) {
  // TODO: It might be more efficient to get a list of existing DoId's in the
  // database by sending a message to the DB server.
  for (auto i = min; i < max; i++) {
    SubscribeChannel(i);
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
    default:
      // Hopefully we managed to unpack the sender...
      Logger::Verbose(
          std::format("[DBSS] Ignoring message: {} from sender: {}",
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
