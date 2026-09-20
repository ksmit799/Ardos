#include "message_director.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>

#include "../clientagent/client_agent.h"
#ifdef ARDOS_WANT_DB_SERVER
#include "../database/database_server.h"
#endif
#include "../net/datagram_iterator.h"
#include "../stateserver/database_state_server.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "../util/metrics.h"
#include "../web/web_panel.h"
#include "channel_subscriber.h"
#include "md_participant.h"
#include "mesh_node.h"

namespace Ardos {

MessageDirector* MessageDirector::_instance = nullptr;

MessageDirector* MessageDirector::Instance() {
  if (_instance == nullptr) {
    _instance = new MessageDirector();
  }

  return _instance;
}

MessageDirector::MessageDirector() {
  spdlog::info("Starting Message Director component...");

  auto config = Config::Instance()->GetNode("message-director");

  // Log configuration.
  spdlog::stdout_color_mt("md");
  if (auto logLevel = config["log-level"]) {
    spdlog::get("md")->set_level(
        Logger::LevelFromString(logLevel.as<std::string>()));
  }

  // Listen configuration.
  if (auto hostParam = config["host"]) {
    _host = hostParam.as<std::string>();
  }
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }

  _listenHandle = g_loop->resource<uvw::tcp_handle>();
  _listenHandle->on<uvw::listen_event>(
      [this](const uvw::listen_event&, uvw::tcp_handle& srv) {
        std::shared_ptr<uvw::tcp_handle> client =
            srv.parent().resource<uvw::tcp_handle>();
        srv.accept(*client);

        // Create a new client for this connected participant.
        auto participant = std::make_shared<MDParticipant>(client);
        participant->Init();
        _participants.insert(participant.get());
      });

  // Initialize metrics.
  InitMetrics();

  _listenHandle->bind(_host, _port);
}

void MessageDirector::StartRoles() {
  auto config = Config::Instance()->GetNode("message-director");

  // The mesh joins us to the rest of the cluster. No mesh section means
  // a cluster of one, everything routes in process.
  if (config["mesh"]) {
    _mesh = new MeshNode();
  }

  if (Config::Instance()->GetBool("want-state-server")) {
    _stateServer = std::make_shared<StateServer>();
    _stateServer->Init();
  }

  if (Config::Instance()->GetBool("want-client-agent")) {
    _clientAgent = std::make_unique<ClientAgent>();
  }

  if (Config::Instance()->GetBool("want-database")) {
#ifdef ARDOS_WANT_DB_SERVER
    _db = std::make_shared<DatabaseServer>();
    _db->Init();
#else
    spdlog::get("md")->error(
        "want-database was set to true but Ardos was "
        "built without ARDOS_WANT_DB_SERVER");
    exit(1);  // NOLINT(concurrency-mt-unsafe)
#endif
  }

  if (Config::Instance()->GetBool("want-db-state-server")) {
    _dbss = std::make_shared<DatabaseStateServer>();
    _dbss->Init();
  }

  if (Config::Instance()->GetBool("want-web-panel")) {
    _webPanel = std::make_unique<WebPanel>();
  }

  // Start listening for incoming participant connections.
  _listenHandle->listen();

  spdlog::get("md")->info("Listening on {}:{}", _host, _port);
}

/**
 * Adds a channel subscriber to start receiving routed messages.
 */
void MessageDirector::AddSubscriber(
    std::shared_ptr<ChannelSubscriber> subscriber) {
  _subscribers.insert(std::move(subscriber));

  if (_subscribersGauge) {
    _subscribersGauge->Increment();
  }
}

/**
 * Removes a channel subscriber. Drops the MD's owning reference; if no other
 * shared_ptr holds it (e.g. a dispatch snapshot), the destructor runs now.
 *
 * Takes a raw pointer because ChannelSubscriber::Shutdown may be invoked
 * from the destructor as a safety net, at which point shared_from_this()
 * is no longer valid. Walking the set comparing by ::get() identity is
 * O(N) but only hit on subscriber teardown, not in the dispatch path.
 */
void MessageDirector::RemoveSubscriber(ChannelSubscriber* subscriber) {
  auto it = std::ranges::find_if(_subscribers, [subscriber](const auto& p) {
    return p.get() == subscriber;
  });
  if (it == _subscribers.end()) {
    return;
  }

  _subscribers.erase(it);
  if (_subscribersGauge) {
    _subscribersGauge->Decrement();
  }
}

void MessageDirector::RouteDatagram(const std::shared_ptr<Datagram>& dg) {
  Route(dg, true);
}

void MessageDirector::RouteLocally(const std::shared_ptr<Datagram>& dg) {
  Route(dg, false);
}

/**
 * The router. One pass unions every interested party across all of the
 * datagram's channels, then each local subscriber gets exactly one
 * HandleDatagram and each interested peer exactly one frame. That union
 * is the at-most-once invariant.
 */
void MessageDirector::Route(const std::shared_ptr<Datagram>& dg, bool toPeers) {
  DatagramIterator dgi(dg);

  uint8_t channelCount = dgi.GetUint8();
  std::vector<uint64_t> channels;
  channels.reserve(channelCount);
  for (uint8_t i = 0; i < channelCount; ++i) {
    channels.push_back(dgi.GetUint64());
  }

  if (_datagramsObservedCounter) {
    _datagramsObservedCounter->Increment();
  }
  if (_datagramsSizeHistogram) {
    _datagramsSizeHistogram->Observe(static_cast<double>(dg->Size()));
  }

  // Local subscribers, points then ranges, the set dedupes a subscriber
  // matching through several channels.
  std::unordered_set<std::shared_ptr<ChannelSubscriber>> interested;
  for (uint64_t channel : channels) {
    if (auto it = ChannelSubscriber::_channelIndex.find(channel);
        it != ChannelSubscriber::_channelIndex.end()) {
      interested.insert(it->second.begin(), it->second.end());
    }
    for (const auto& entry : ChannelSubscriber::_rangeIndex) {
      if (channel >= entry.lo && channel <= entry.hi && entry.sub) {
        interested.insert(entry.sub);
      }
    }
  }

  // Interested peers, one frame per link no matter how many channels
  // matched, and never onward from a routed datagram, the mesh has no
  // relay.
  std::unordered_set<MeshLink*> links;
  if (toPeers && _mesh) {
    _mesh->CollectLinks(channels, links);
    // Send order across links carries no meaning, per link FIFO is the
    // only ordering the protocol promises.
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order)
    for (MeshLink* link : links) {
      link->Send(dg);
    }
    if (_remoteSendsCounter) {
      _remoteSendsCounter->Increment(static_cast<double>(links.size()));
    }
    if (_fanoutLinksHistogram) {
      _fanoutLinksHistogram->Observe(static_cast<double>(links.size()));
    }
  }

  if (interested.empty()) {
    return;
  }

  if (_datagramsProcessedCounter) {
    _datagramsProcessedCounter->Increment();
  }
  if (_localDeliveriesCounter) {
    _localDeliveriesCounter->Increment(static_cast<double>(interested.size()));
  }

  spdlog::get("md")->trace("Route matched={} links={} size={}B",
                           interested.size(), links.size(), dg->Size());

  // Snapshot into a vector. Shared_ptr copies keep every iterated
  // subscriber alive across the loop even if a handler triggers
  // RemoveSubscriber or UnsubscribeChannel for one of its peers.
  std::vector<std::shared_ptr<ChannelSubscriber>> snapshot(interested.begin(),
                                                           interested.end());
  for (const auto& subscriber : snapshot) {
    subscriber->HandleDatagram(dg);
  }
}

void MessageDirector::BroadcastAddChannel(uint64_t channel) {
  if (_mesh) {
    _mesh->BroadcastAddChannel(channel);
  }
}

void MessageDirector::BroadcastRemoveChannel(uint64_t channel) {
  if (_mesh) {
    _mesh->BroadcastRemoveChannel(channel);
  }
}

void MessageDirector::BroadcastAddRange(uint64_t lo, uint64_t hi) {
  if (_mesh) {
    _mesh->BroadcastAddRange(lo, hi);
  }
}

void MessageDirector::BroadcastRemoveRange(uint64_t lo, uint64_t hi) {
  if (_mesh) {
    _mesh->BroadcastRemoveRange(lo, hi);
  }
}

void MessageDirector::AddPostRemove(uint64_t sender,
                                    const std::shared_ptr<Datagram>& dg) {
  if (_mesh) {
    _mesh->AddLocalPostRemove(sender, dg);
  }
}

void MessageDirector::ClearPostRemoves(uint64_t sender) {
  if (_mesh) {
    _mesh->ClearLocalPostRemoves(sender);
  }
}

/**
 * Called when a participant connects.
 */
void MessageDirector::ParticipantJoined() {
  if (_participantsGauge) {
    _participantsGauge->Increment();
  }
}

/**
 * Called when a participant disconnects.
 */
void MessageDirector::ParticipantLeft(MDParticipant* participant) {
  if (_participantsGauge) {
    _participantsGauge->Decrement();
  }

  _participants.erase(participant);
}

/**
 * Initializes metrics collection for the message director.
 */
void MessageDirector::InitMetrics() {
  // Make sure we want to collect metrics on this cluster.
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  auto& packetsBuilder = prometheus::BuildCounter()
                             .Name("md_observed_datagrams_total")
                             .Help("Number of datagrams observed")
                             .Register(*registry);

  auto& datagramsBuilder = prometheus::BuildCounter()
                               .Name("md_handled_datagrams_total")
                               .Help("Number of datagrams handled")
                               .Register(*registry);

  auto& datagramsSizeBuilder = prometheus::BuildHistogram()
                                   .Name("md_datagrams_bytes_size")
                                   .Help("Bytes size of handled datagrams")
                                   .Register(*registry);

  auto& fanoutBuilder = prometheus::BuildHistogram()
                            .Name("md_route_fanout_links")
                            .Help("Peer links matched per routed datagram")
                            .Register(*registry);

  auto& localBuilder = prometheus::BuildCounter()
                           .Name("md_route_local_deliveries_total")
                           .Help("Datagram deliveries to local subscribers")
                           .Register(*registry);

  auto& remoteBuilder = prometheus::BuildCounter()
                            .Name("md_route_remote_sends_total")
                            .Help("Datagram sends to mesh peers")
                            .Register(*registry);

  auto& subscribersBuilder = prometheus::BuildGauge()
                                 .Name("md_subscribers_size")
                                 .Help("Number of registered subscribers")
                                 .Register(*registry);

  auto& participantsBuilder = prometheus::BuildGauge()
                                  .Name("md_participants_size")
                                  .Help("Number of connected participants")
                                  .Register(*registry);

  _datagramsObservedCounter = &packetsBuilder.Add({});
  _datagramsProcessedCounter = &datagramsBuilder.Add({});
  _datagramsSizeHistogram = &datagramsSizeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{1, 4, 16, 64, 256, 1024, 4096,
                                                  16384, 65536});
  _fanoutLinksHistogram = &fanoutBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{0, 1, 2, 4, 8, 16, 32});
  _localDeliveriesCounter = &localBuilder.Add({});
  _remoteSendsCounter = &remoteBuilder.Add({});
  _subscribersGauge = &subscribersBuilder.Add({});
  _participantsGauge = &participantsBuilder.Add({});
}

void MessageDirector::HandleWeb(ws28::Client* client, nlohmann::json& data) {
  // Build up an array of connected participants.
  nlohmann::json participantInfo = nlohmann::json::array();
  for (const auto& participant : _participants) {
    participantInfo.push_back({
        {"name", participant->GetName()},
        {"ip", participant->GetRemoteAddress().ip},
        {"port", participant->GetRemoteAddress().port},
        {"channels", participant->GetLocalChannels().size()},
        {"postRemoves", participant->GetPostRemovesCount()},
    });
  }

  // Mesh peers, empty for a cluster of one.
  nlohmann::json peerInfo = nlohmann::json::array();
  if (_mesh) {
    for (const auto& peer : _mesh->GetPeerInfo()) {
      peerInfo.push_back({
          {"nodeId", peer.nodeId},
          {"addr", peer.addr},
          {"rttMs", peer.rttMs},
      });
    }
  }

  WebPanel::Send(client, {
                             {"type", "md"},
                             {"success", true},
                             {"listenIp", _host},
                             {"listenPort", _port},
                             {"meshNodeId", _mesh ? _mesh->GetNodeId() : 0},
                             {"meshPeers", peerInfo},
                             {"participants", participantInfo},
                         });
}

}  // namespace Ardos
