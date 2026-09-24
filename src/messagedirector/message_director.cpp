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

  // Load balanced uberdog channels become shared groups, the router
  // delivers to exactly one member. Filed before the mesh starts so the
  // join snapshot advertises them correctly.
  std::unordered_set<uint64_t> shared;
  for (auto uberdog : Config::Instance()->GetNode("uberdogs")) {
    if (auto lbParam = uberdog["load-balanced"];
        lbParam && lbParam.as<bool>()) {
      shared.insert(uberdog["id"].as<uint64_t>());
    }
  }
  ChannelSubscriber::SetSharedChannels(std::move(shared));

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
  // Shared channels route to exactly one group member via the rendezvous
  // pick, never the broadcast union. The peer table counts too, a peer
  // declaring a channel shared wins over a mismatched local config.
  std::vector<uint64_t> shared;
  for (uint8_t i = 0; i < channelCount; ++i) {
    uint64_t channel = dgi.GetUint64();
    if (ChannelSubscriber::IsSharedChannel(channel) ||
        (_mesh && _mesh->SumSharedPeers(channel) > 0)) {
      shared.push_back(channel);
    } else {
      channels.push_back(channel);
    }
  }

  // The sender channel sits after the channel list, picks key on it so
  // every message from one client lands on the same member.
  uint64_t sender = 0;
  if (!shared.empty() &&
      dg->Size() >= sizeof(uint8_t) + ((static_cast<size_t>(channelCount) + 1) *
                                       sizeof(uint64_t))) {
    sender = dgi.GetUint64();
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
  }

  // Shared picks join the union sets, so a winner also matched by a
  // normal channel still gets exactly one delivery.
  for (uint64_t channel : shared) {
    PickSharedMember(channel, sender, toPeers, interested, links);
  }

  if (toPeers && _mesh) {
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

// Splitmix64 finalizer, mixes a candidate key into a rendezvous weight.
static uint64_t Mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27U)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31U);
}

static uint64_t RendezvousWeight(uint64_t sender, uint64_t channel,
                                 uint32_t nodeId, uint16_t slot) {
  uint64_t candidate = (static_cast<uint64_t>(nodeId) << 16U) | slot;
  return Mix64(sender ^ Mix64(channel) ^ Mix64(candidate));
}

/**
 * Picks the one shared group member a datagram goes to. Every candidate
 * member, local and remote, hashes (sender, channel, member) and the
 * highest weight wins, so all instances agree on the winner and a member
 * change only remaps the senders that hashed onto it.
 */
void MessageDirector::PickSharedMember(
    uint64_t channel, uint64_t sender, bool toPeers,
    std::unordered_set<std::shared_ptr<ChannelSubscriber>>& interested,
    std::unordered_set<MeshLink*>& links) {
  uint32_t nodeId = _mesh ? _mesh->GetNodeId() : 0;

  bool found = false;
  uint64_t bestWeight = 0;
  std::shared_ptr<ChannelSubscriber> bestLocal;
  MeshLink* bestLink = nullptr;
  uint32_t bestNode = 0;
  uint16_t bestSlot = 0;

  if (auto it = ChannelSubscriber::_sharedLocal.find(channel);
      it != ChannelSubscriber::_sharedLocal.end()) {
    for (size_t slot = 0; slot < it->second.size(); ++slot) {
      uint64_t weight = RendezvousWeight(sender, channel, nodeId,
                                         static_cast<uint16_t>(slot));
      if (!found || weight > bestWeight) {
        found = true;
        bestWeight = weight;
        bestLocal = it->second[slot];
        bestLink = nullptr;
        bestNode = nodeId;
        bestSlot = static_cast<uint16_t>(slot);
      }
    }
  }

  // A frame from a peer never hops again, so remote candidates only
  // enter the pick on the publishing instance. Each member a peer holds
  // is one candidate, an instance with more members draws more picks.
  if (toPeers && _mesh) {
    std::vector<MeshNode::SharedPeer> peers;
    _mesh->CollectSharedPeers(channel, peers);
    for (const auto& peer : peers) {
      for (uint16_t slot = 0; slot < peer.count; ++slot) {
        uint64_t weight = RendezvousWeight(sender, channel, peer.nodeId, slot);
        if (!found || weight > bestWeight) {
          found = true;
          bestWeight = weight;
          bestLocal = nullptr;
          bestLink = peer.link;
          bestNode = peer.nodeId;
          bestSlot = slot;
        }
      }
    }
  }

  if (!found) {
    spdlog::get("md")->warn(
        "Shared channel {} has no members, dropping datagram", channel);
    if (_sharedNoMemberFamily) {
      auto& counter = _sharedNoMemberCounters[channel];
      if (!counter) {
        counter =
            &_sharedNoMemberFamily->Add({{"channel", std::to_string(channel)}});
      }
      counter->Increment();
    }
    return;
  }

  if (!bestLocal) {
    links.insert(bestLink);
    return;
  }
  interested.insert(bestLocal);

  // Counted only where the datagram lands, a remote winner is counted
  // by the instance that delivers it, once cluster wide per datagram.
  if (_sharedPicksFamily) {
    std::string member =
        std::to_string(bestNode) + ":" + std::to_string(bestSlot);
    auto& counter = _sharedPickCounters[{channel, member}];
    if (!counter) {
      counter = &_sharedPicksFamily->Add(
          {{"channel", std::to_string(channel)}, {"member", member}});
    }
    counter->Increment();
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

void MessageDirector::BroadcastSharedChannel(uint64_t channel, uint16_t count) {
  if (_mesh) {
    _mesh->BroadcastSharedChannel(channel, count);
  }
  UpdateSharedMembers(channel);
}

/**
 * Refreshes the member count gauge for a shared channel, local members
 * plus every member advertised by peers.
 */
void MessageDirector::UpdateSharedMembers(uint64_t channel) {
  if (!_sharedMembersFamily) {
    return;
  }

  auto total = static_cast<uint32_t>(0);
  if (auto it = ChannelSubscriber::_sharedLocal.find(channel);
      it != ChannelSubscriber::_sharedLocal.end()) {
    total += static_cast<uint32_t>(it->second.size());
  }
  if (_mesh) {
    total += _mesh->SumSharedPeers(channel);
  }

  auto& gauge = _sharedMembersGauges[channel];
  if (!gauge) {
    gauge = &_sharedMembersFamily->Add({{"channel", std::to_string(channel)}});
  }
  gauge->Set(total);
}

void MessageDirector::AddPostRemove(uint32_t owner, uint64_t sender,
                                    const std::shared_ptr<Datagram>& dg) {
  if (_mesh) {
    _mesh->AddLocalPostRemove(owner, sender, dg);
  }
}

void MessageDirector::ClearPostRemoves(uint32_t owner, uint64_t sender) {
  if (_mesh) {
    _mesh->ClearLocalPostRemoves(owner, sender);
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

  _sharedMembersFamily =
      &prometheus::BuildGauge()
           .Name("md_shared_members")
           .Help("Members in a shared channel group, cluster wide")
           .Register(*registry);

  _sharedPicksFamily = &prometheus::BuildCounter()
                            .Name("md_shared_picks_total")
                            .Help("Rendezvous picks per shared group member")
                            .Register(*registry);

  _sharedNoMemberFamily =
      &prometheus::BuildCounter()
           .Name("md_shared_no_member_total")
           .Help("Datagrams dropped on a shared channel with no members")
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
