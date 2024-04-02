#include "state_server.h"

#include <dcClass.h>

#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "../util/metrics.h"
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
  SubscribeChannel(BCHAN_STATESERVERS);

  // Initialize metrics.
  InitMetrics();
}

void StateServer::RemoveDistributedObject(const uint32_t &doId) {
  _distObjs.erase(doId);

  if (_objectsGauge) {
    _objectsGauge->Decrement();
  }
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
      HandleDeleteAI(dgi, sender);
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
  if (_distObjs.contains(doId)) {
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
  _distObjs[doId] =
      new DistributedObject(this, doId, parentId, zoneId, dcClass, dgi, other);

  if (_objectsGauge) {
    _objectsGauge->Increment();
  }

  if (_objectsSizeHistogram) {
    _objectsSizeHistogram->Observe((double)_distObjs[doId]->Size());
  }
}

void StateServer::HandleDeleteAI(DatagramIterator &dgi,
                                 const uint64_t &sender) {
  uint64_t aiChannel = dgi.GetUint64();

  Logger::Info(std::format("[SS] AI '{}' going offline... Deleting objects.",
                           aiChannel));

  std::unordered_set<uint64_t> targets;
  for (const auto &distObj : _distObjs) {
    if (distObj.second->GetAI() == aiChannel &&
        distObj.second->IsAIExplicitlySet()) {
      targets.insert(distObj.first);
    }
  }

  auto dg = std::make_shared<Datagram>(targets, sender,
                                       STATESERVER_DELETE_AI_OBJECTS);
  dg->AddUint64(aiChannel);
  PublishDatagram(dg);
}

void StateServer::InitMetrics() {
  // Make sure we want to collect metrics on this cluster.
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  auto &objectsBuilder = prometheus::BuildGauge()
                             .Name("ss_objects_size")
                             .Help("Number of loaded distributed objects")
                             .Register(*registry);

  auto &objectsSizeBuilder =
      prometheus::BuildHistogram()
          .Name("ss_objects_bytes_size")
          .Help("Byte-size of loaded distributed objects")
          .Register(*registry);

  _objectsGauge = &objectsBuilder.Add({});
  _objectsSizeHistogram = &objectsSizeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{0, 4, 16, 64, 256, 1024, 4096,
                                                  16384, 65536});
}

void StateServer::HandleWeb(ws28::Client *client, nlohmann::json &data) {}

} // namespace Ardos
