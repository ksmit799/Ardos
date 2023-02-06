#include "state_server.h"

#include <dcClass.h>

#include "../messagedirector/message_director.h"
#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "distributed_object.h"

namespace Ardos {

StateServer::StateServer() {
  Logger::Info("Starting State Server component...");

  // State Server configuration.
  auto config = Config::Instance()->GetNode("state-server");
  if (!config["channel"]) {
    Logger::Error("[SS] Missing or invalid channel!");
    exit(1);
  }

  _channel = config["channel"].as<uint64_t>();

  auto globalChannel = MessageDirector::Instance()->GetGlobalChannel();
  auto localQueue = MessageDirector::Instance()->GetLocalQueue();

  // Listen to the State Server channel.
  globalChannel->bindQueue(kGlobalExchange, localQueue,
                           std::to_string(_channel));

  globalChannel->consume(localQueue)
      .onReceived([this](const AMQP::Message &message, uint64_t deliveryTag,
                         bool redelivered) {
        // We've received a message from the MD. Handle it.
        auto dg = std::make_shared<Datagram>(
            reinterpret_cast<const uint8_t *>(message.body()),
            message.bodySize());
        HandleDatagram(dg);
      });
}

void StateServer::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Skip MD routing headers.
  dgi.SkipHeaders();

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
