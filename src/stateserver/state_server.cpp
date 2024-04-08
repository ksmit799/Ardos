#include "state_server.h"

#include <dcClass.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "../util/metrics.h"
#include "../web/web_panel.h"
#include "distributed_object.h"

namespace Ardos {

StateServer::StateServer() : ChannelSubscriber() {
  spdlog::info("Starting State Server component...");

  // State Server configuration.
  auto config = Config::Instance()->GetNode("state-server");

  // Log configuration.
  spdlog::stdout_color_mt("ss");
  if (auto logLevel = config["log-level"]) {
    spdlog::get("ss")->set_level(
        Logger::LevelFromString(logLevel.as<std::string>()));
  }

  if (!config["channel"]) {
    spdlog::get("ss")->error("Missing or invalid channel!");
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
      spdlog::get("ss")->warn("Received unknown message: {} from sender: {}",
                              msgType, sender);
    }
  } catch (const DatagramIteratorEOF &) {
    spdlog::get("ss")->error("Received a truncated datagram!");
  }
}

void StateServer::HandleGenerate(DatagramIterator &dgi, const bool &other) {
  uint32_t doId = dgi.GetUint32();
  uint32_t parentId = dgi.GetUint32();
  uint32_t zoneId = dgi.GetUint32();
  uint16_t dcId = dgi.GetUint16();

  // Make sure we don't have a duplicate generate.
  if (_distObjs.contains(doId)) {
    spdlog::get("ss")->error("Received duplicate generate for DoId: {}", doId);
    return;
  }

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class(dcId);
  if (!dcClass) {
    spdlog::get("ss")->error(
        "Received generate for unknown distributed class: {}", dcId);
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

  spdlog::get("ss")->info("AI '{}' going offline... Deleting objects.",
                          aiChannel);

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

void StateServer::HandleWeb(ws28::Client *client, nlohmann::json &data) {
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
                               {"type", "ss:init"},
                               {"success", true},
                               {"channel", _channel},
                               {"distObjs", distObjInfo},
                           });
  } else if (data["msg"] == "distobj") {
    auto doId = data["doId"].template get<uint32_t>();

    // Try to find a matching Distributed Object for the provided DoId.
    if (!_distObjs.contains(doId)) {
      WebPanel::Send(client, {
                                 {"type", "ss:distobj"},
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
      for (const auto &zoneDoId : zoneData.second) {
        // Try to get the DClass name for the zone object.
        auto clsName = _distObjs.contains(zoneDoId)
                           ? _distObjs[zoneDoId]->GetDClass()->get_name()
                           : "Unknown";

        zoneObjs[std::to_string(zoneData.first)].push_back(
            {{"doId", zoneDoId}, {"clsName", clsName}});
      }
    }

    WebPanel::Send(client, {
                               {"type", "ss:distobj"},
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
