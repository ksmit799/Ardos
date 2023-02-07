#include "state_server.h"

#include <dcClass.h>

#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "distributed_object.h"

namespace Ardos {

StateServer::StateServer() : ChannelSubscriber() {
  Logger::Info("Starting State Server component...");

  // State Server configuration.
  auto config = Config::Instance()->GetNode("state-server");
  if (!config["channel"]) {
    Logger::Error("[SS] Missing or invalid channel!");
    exit(1);
  }

  // Start listening to our channel.
  _channel = config["channel"].as<uint64_t>();
  SubscribeChannel(_channel);
}

void StateServer::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Skip MD routing headers.
  dgi.SeekPayload();

  try {
    uint64_t sender = dgi.GetUint64();
    uint16_t msgType = dgi.GetUint16();
    switch (msgType) {
    case STATESERVER_CREATE_OBJECT_WITH_REQUIRED:
      HandleGenerate(dgi, false);
      break;
    case STATESERVER_CREATE_OBJECT_WITH_REQUIRED_OTHER:
      HandleGenerate(dgi, true);
      break;
    case STATESERVER_DELETE_AI_OBJECTS:
      break;
    default:
      // Hopefully we managed to unpack the sender...
      Logger::Warn(
          std::format("[SS] Received unknown message: {} from sender: {}",
                      msgType, sender));
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error("[SS] Received a truncated datagram!");
  }
}

void StateServer::HandleGenerate(DatagramIterator &dgi, const bool &other) {
  uint32_t doId = dgi.GetUint32();
  uint32_t parentId = dgi.GetUint32();
  uint32_t zoneId = dgi.GetUint32();
  uint16_t dcId = dgi.GetUint16();

  // Make sure we don't have a duplicate generate.
  if (_dist_objs.contains(doId)) {
    Logger::Error(
        std::format("[SS] Received duplicate generate for DoId: {}", doId));
    return;
  }

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class(dcId);
  if (!dcClass) {
    Logger::Error(std::format(
        "[SS] Received generate for unknown distributed class: {}", dcId));
    return;
  }

  // Create the distributed object.
  _dist_objs[doId] =
      new DistributedObject(this, doId, parentId, zoneId, dcClass, dgi, other);
}

} // namespace Ardos
