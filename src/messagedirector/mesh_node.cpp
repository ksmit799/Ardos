#include "mesh_node.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

#include "../net/address_utils.h"
#include "../net/datagram_iterator.h"
#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/metrics.h"
#include "channel_subscriber.h"
#include "message_director.h"

namespace Ardos {

// Splits a "host:port" address, returns false if it doesn't parse.
static bool ParseAddr(const std::string& addr, std::string& host, int& port) {
  auto pos = addr.rfind(':');
  if (pos == std::string::npos || pos == 0 || pos == addr.size() - 1) {
    return false;
  }
  host = addr.substr(0, pos);
  try {
    port = std::stoi(addr.substr(pos + 1));
  } catch (const std::exception&) {
    return false;
  }
  return port > 0 && port <= 0xffff;
}

MeshNode::MeshNode() {
  auto config = Config::Instance()->GetNode("message-director")["mesh"];

  if (auto nodeIdParam = config["node-id"]) {
    _nodeId = nodeIdParam.as<uint32_t>();
  } else {
    spdlog::get("md")->error("Mesh config requires a unique node-id");
    exit(1);  // NOLINT(concurrency-mt-unsafe)
  }
  if (auto hostParam = config["host"]) {
    _host = hostParam.as<std::string>();
  }
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }
  if (auto heartbeatParam = config["heartbeat-interval"]) {
    _heartbeatMs = heartbeatParam.as<uint32_t>();
  }
  if (auto missedParam = config["missed-heartbeats"]) {
    _missedHeartbeats = missedParam.as<uint32_t>();
  }

  // The epoch marks this incarnation, a reconnecting peer with a new
  // epoch is a new peer, never a resume.
  _epoch = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());

  InitMetrics();
  Listen();

  // Dial our seeds, one live seed is enough to learn everyone else.
  if (auto seedsParam = config["seeds"]) {
    for (const auto& seed : seedsParam.as<std::vector<std::string>>()) {
      EnsureDial(seed);
      if (auto it = _dials.find(seed); it != _dials.end()) {
        it->second.seed = true;
      }
    }
  }

  _tickTimer = g_loop->resource<uvw::timer_handle>();
  _tickTimer->on<uvw::timer_event>(
      [this](const uvw::timer_event&, uvw::timer_handle&) { Tick(); });
  _tickTimer->start(uvw::timer_handle::time{_heartbeatMs},
                    uvw::timer_handle::time{_heartbeatMs});

  spdlog::get("md")->info("Mesh node {} listening on {}:{}", _nodeId, _host,
                          _port);
}

uint64_t MeshNode::SteadyMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

std::shared_ptr<Datagram> MeshNode::MakeControl(uint16_t msgType) {
  auto dg = std::make_shared<Datagram>();
  dg->AddUint8(MESH_CONTROL_HEADER);
  dg->AddUint16(msgType);
  return dg;
}

std::vector<MeshNode::PeerInfo> MeshNode::GetPeerInfo() const {
  std::vector<PeerInfo> info;
  info.reserve(_peers.size());
  for (const auto& [nodeId, link] : _peers) {
    info.push_back(
        {.nodeId = nodeId, .addr = link->ListenAddr(), .rttMs = link->RttMs()});
  }
  return info;
}

void MeshNode::Listen() {
  _listenHandle = g_loop->resource<uvw::tcp_handle>();

  _listenHandle->on<uvw::listen_event>(
      [this](const uvw::listen_event&, uvw::tcp_handle& srv) {
        std::shared_ptr<uvw::tcp_handle> client =
            srv.parent().resource<uvw::tcp_handle>();
        srv.accept(*client);

        // The dialer speaks first, we wait for its HELLO.
        auto link = std::make_shared<MeshLink>(this, client, false, "");
        _links[link.get()] = link;
      });

  _listenHandle->on<uvw::error_event>(
      [](const uvw::error_event& event, uvw::tcp_handle&) {
        spdlog::get("md")->error("Mesh listen error: {}", event.what());
        exit(1);  // NOLINT(concurrency-mt-unsafe)
      });

  _listenHandle->bind(_host, _port);
  _listenHandle->listen();
}

void MeshNode::EnsureDial(const std::string& addr) {
  std::string host;
  int port = 0;
  if (!ParseAddr(addr, host, port)) {
    spdlog::get("md")->error("Mesh address doesn't parse: {}", addr);
    return;
  }

  auto& dial = _dials[addr];
  dial.host = host;
  dial.port = port;
  if (dial.backoffMs == 0) {
    dial.backoffMs = kDialBackoffStartMs;
  }

  if (dial.self) {
    return;
  }
  // Already connected to a live peer at this address, or mid dial.
  for (const auto& [nodeId, link] : _peers) {
    if (link->ListenAddr() == addr || link->DialedAddr() == addr) {
      return;
    }
  }
  if (dial.connecting || (dial.retryTimer && dial.retryTimer->active())) {
    return;
  }

  // A fresh reason to dial resets the failure budget.
  dial.failures = 0;
  DialNow(addr);
}

void MeshNode::DialNow(const std::string& addr) {
  auto& dial = _dials[addr];

  if (_dialAttemptsCounter) {
    _dialAttemptsCounter->Increment();
  }

  auto handle = g_loop->resource<uvw::tcp_handle>();
  dial.connecting = handle;

  handle->on<uvw::connect_event>(
      [this, addr](const uvw::connect_event&, uvw::tcp_handle& tcp) {
        auto& dial = _dials[addr];
        auto socket = dial.connecting;
        dial.connecting = nullptr;

        auto link = std::make_shared<MeshLink>(this, socket, true, addr);
        _links[link.get()] = link;
        link->SendHello();
      });

  handle->on<uvw::error_event>(
      [this, addr](const uvw::error_event&, uvw::tcp_handle& tcp) {
        auto& dial = _dials[addr];
        if (dial.connecting) {
          dial.connecting->close();
          dial.connecting = nullptr;
        }
        ScheduleRetry(addr);
      });

  spdlog::get("md")->debug("Dialing mesh peer at {}", addr);
  handle->connect(AddressUtils::resolve_host(g_loop, dial.host, dial.port),
                  dial.port);
}

void MeshNode::ScheduleRetry(const std::string& addr) {
  auto& dial = _dials[addr];

  if (!dial.seed && ++dial.failures >= kMaxLearnedDialFailures) {
    spdlog::get("md")->debug("Giving up on mesh address {}", addr);
    return;
  }

  if (!dial.retryTimer) {
    dial.retryTimer = g_loop->resource<uvw::timer_handle>();
    dial.retryTimer->on<uvw::timer_event>(
        [this, addr](const uvw::timer_event&, uvw::timer_handle&) {
          DialNow(addr);
        });
  }
  dial.retryTimer->start(uvw::timer_handle::time{dial.backoffMs},
                         uvw::timer_handle::time{0});
  dial.backoffMs = std::min(dial.backoffMs * 2, kDialBackoffMaxMs);
}

void MeshNode::OnLinkHello(MeshLink* link) {
  uint32_t nodeId = link->NodeId();

  if (nodeId == _nodeId) {
    // Matching epoch means this is literally us, we dialed our own listen
    // address, common when every instance shares one seeds list. A
    // different epoch is another process claiming our node id.
    if (link->Epoch() == _epoch) {
      spdlog::get("md")->debug("Dialed ourselves, dropping");
    } else {
      spdlog::get("md")->error(
          "Another instance is using our node id ({}), check for a "
          "duplicate node-id in the cluster config",
          _nodeId);
    }
    // Either way, stop dialing this address.
    if (link->Outbound() && _dials.contains(link->DialedAddr())) {
      _dials[link->DialedAddr()].self = true;
    }
    CloseLink(link);
    return;
  }

  bool sameIncarnation = false;
  if (auto it = _peers.find(nodeId); it != _peers.end()) {
    MeshLink* existing = it->second;
    if (existing->Epoch() == link->Epoch()) {
      // Both sides dialed at once, keep the link dialed by the lower id,
      // both sides pick the same winner.
      uint32_t newDialer = link->Outbound() ? _nodeId : nodeId;
      uint32_t oldDialer = existing->Outbound() ? _nodeId : nodeId;
      if (oldDialer <= newDialer) {
        CloseLink(link);
        return;
      }
      sameIncarnation = true;
      CloseLink(existing);
    } else {
      // The peer restarted before its old link died.
      spdlog::get("md")->info("Mesh peer {} returned with a new epoch", nodeId);
      CloseLink(existing);
    }
  }

  if (!sameIncarnation) {
    // A rejoin is proof the old incarnation died, its bundle fires
    // without waiting for corroboration.
    if (_bundles.contains(nodeId) && !_fired.contains(nodeId) &&
        IsLowestLive()) {
      FireBundle(nodeId, "old incarnation replaced");
    }
    _bundles.erase(nodeId);
    _fired.erase(nodeId);
    _lowestAtDeath.erase(nodeId);
  }

  _peers[nodeId] = link;
  _everPeers.insert(nodeId);

  // A live link means no redial for this address, reset the backoff
  // for next time.
  for (const std::string& addr : {link->ListenAddr(), link->DialedAddr()}) {
    if (addr.empty()) {
      continue;
    }
    auto dialIt = _dials.find(addr);
    if (dialIt != _dials.end()) {
      dialIt->second.backoffMs = kDialBackoffStartMs;
      dialIt->second.failures = 0;
      if (dialIt->second.retryTimer) {
        dialIt->second.retryTimer->stop();
      }
    }
  }

  if (_peersGauge) {
    _peersGauge->Set(static_cast<double>(_peers.size()));
  }
  SetPeerVisibleMetric(nodeId, true);

  spdlog::get("md")->info("Mesh peer {} joined from {} (epoch {})", nodeId,
                          link->ListenAddr(), link->Epoch());

  SendPeerState(link);

  if (!sameIncarnation) {
    // Gossip the newcomer so everyone dials it.
    auto dg = MakeControl(MESH_PEERS);
    dg->AddUint16(1);
    dg->AddUint32(nodeId);
    dg->AddString(link->ListenAddr());
    for (const auto& [peerId, peerLink] : _peers) {
      if (peerId != nodeId) {
        peerLink->Send(dg);
      }
    }
  }
}

void MeshNode::SendPeerState(MeshLink* link) {
  // Everyone we know, so one live seed is enough to join the full mesh.
  if (_peers.size() > 1) {
    auto peersDg = MakeControl(MESH_PEERS);
    peersDg->AddUint16(static_cast<uint16_t>(_peers.size() - 1));
    for (const auto& [peerId, peerLink] : _peers) {
      if (peerId == link->NodeId()) {
        continue;
      }
      peersDg->AddUint32(peerId);
      peersDg->AddString(peerLink->ListenAddr());
    }
    link->Send(peersDg);
  }

  // Everything we subscribe to. Snapshots are chunked, a datagram caps
  // at 64KB and a busy state server holds a point subscription for every
  // live object. Only the first chunk resets the peers view of us.
  const auto& channels = ChannelSubscriber::GetAdvertisedChannels();
  const auto& ranges = ChannelSubscriber::GetAdvertisedRanges();
  constexpr size_t kChunkSize = 4096;
  auto it = channels.begin();
  bool first = true;
  do {
    auto dg = MakeControl(MESH_SNAPSHOT);
    dg->AddUint8(first ? 1 : 0);

    size_t count = 0;
    auto countAt = it;
    while (countAt != channels.end() && count < kChunkSize) {
      ++countAt;
      ++count;
    }
    dg->AddUint32(static_cast<uint32_t>(count));
    while (it != countAt) {
      dg->AddUint64(it->first);
      ++it;
    }

    // Ranges are few and ride the first chunk.
    dg->AddUint32(first ? static_cast<uint32_t>(ranges.size()) : 0);
    if (first) {
      for (const auto& [range, count2] : ranges) {
        dg->AddUint64(range.first);
        dg->AddUint64(range.second);
      }
    }

    link->Send(dg);
    first = false;
  } while (it != channels.end());

  // Our cleanup bundle, they hold a copy in case we die uncleanly.
  for (const auto& [key, dgs] : _localBundle) {
    for (const auto& postRemove : dgs) {
      auto dg = MakeControl(MESH_ADD_POST_REMOVE);
      dg->AddUint32(key.first);
      dg->AddUint64(key.second);
      dg->AddBlob(postRemove->GetData(), postRemove->Size());
      link->Send(dg);
    }
  }
}

void MeshNode::CloseLink(MeshLink* link) {
  if (link->_removed) {
    return;
  }
  link->_removed = true;

  RemoveLinkEntries(link);
  if (auto it = _peers.find(link->NodeId());
      it != _peers.end() && it->second == link) {
    _peers.erase(it);
  }

  // Keep the link alive until this scope ends, Close tears the socket down.
  std::shared_ptr<MeshLink> self;
  if (auto it = _links.find(link); it != _links.end()) {
    self = it->second;
    _links.erase(it);
  }
  link->Close();
}

void MeshNode::OnLinkDown(MeshLink* link) {
  if (link->_removed) {
    return;
  }
  link->_removed = true;

  std::shared_ptr<MeshLink> self;
  if (auto it = _links.find(link); it != _links.end()) {
    self = it->second;
    _links.erase(it);
  }
  RemoveLinkEntries(link);
  link->Close();

  uint32_t nodeId = link->NodeId();
  bool wasPeer = false;
  if (auto it = _peers.find(nodeId); it != _peers.end() && it->second == link) {
    _peers.erase(it);
    wasPeer = true;
  }

  if (wasPeer) {
    _peerVisibility.erase(nodeId);
    // Remember who would have fired at this moment, used to spot refires.
    uint32_t lowest = _nodeId;
    if (!_peers.empty()) {
      lowest = std::min(lowest, _peers.begin()->first);
    }
    _lowestAtDeath[nodeId] = lowest;

    if (_peersGauge) {
      _peersGauge->Set(static_cast<double>(_peers.size()));
    }
    SetPeerVisibleMetric(nodeId, false);
    if (_peerLossesCounter) {
      _peerLossesCounter->Increment();
    }

    spdlog::get("md")->warn("Mesh peer {} is down", nodeId);

    // The link is dead for good, but the peer may come back, a redial
    // that succeeds is a brand new join.
    if (!link->ListenAddr().empty()) {
      EnsureDial(link->ListenAddr());
    } else if (!link->DialedAddr().empty()) {
      EnsureDial(link->DialedAddr());
    }

    CheckIsolation();
  } else if (link->Outbound() && !link->DialedAddr().empty()) {
    // A dial that never finished its handshake, keep trying.
    ScheduleRetry(link->DialedAddr());
  }
}

void MeshNode::RemoveLinkEntries(MeshLink* link) {
  for (uint64_t channel : link->_channels) {
    auto it = _peerChannels.find(channel);
    if (it == _peerChannels.end()) {
      continue;
    }
    it->second.erase(link);
    if (it->second.empty()) {
      _peerChannels.erase(it);
    }
  }
  link->_channels.clear();

  std::erase_if(_peerRanges,
                [link](const RangeEntry& e) { return e.link == link; });
  link->_ranges.clear();
}

void MeshNode::CheckIsolation() {
  // Losing one peer is that peers problem, losing every peer we ever had
  // means our own network is gone. Exit rather than continuing to run
  // isolated from the rest of the network.
  if (_peers.empty() && _everPeers.size() >= 2) {
    spdlog::get("md")->critical(
        "Lost every mesh peer, we're isolated, shutting down");
    exit(1);  // NOLINT(concurrency-mt-unsafe)
  }
}

void MeshNode::PeerAddChannel(MeshLink* link, uint64_t channel) {
  if (link->_channels.insert(channel).second) {
    _peerChannels[channel].insert(link);
  }
}

void MeshNode::PeerRemoveChannel(MeshLink* link, uint64_t channel) {
  if (link->_channels.erase(channel)) {
    auto it = _peerChannels.find(channel);
    if (it != _peerChannels.end()) {
      it->second.erase(link);
      if (it->second.empty()) {
        _peerChannels.erase(it);
      }
    }
  }
}

void MeshNode::PeerAddRange(MeshLink* link, uint64_t lo, uint64_t hi) {
  link->_ranges.emplace_back(lo, hi);
  _peerRanges.push_back({.lo = lo, .hi = hi, .link = link});
}

void MeshNode::PeerRemoveRange(MeshLink* link, uint64_t lo, uint64_t hi) {
  auto range = std::make_pair(lo, hi);
  if (auto it = std::ranges::find(link->_ranges, range);
      it != link->_ranges.end()) {
    link->_ranges.erase(it);
  }
  auto it = std::ranges::find_if(_peerRanges, [&](const RangeEntry& e) {
    return e.link == link && e.lo == lo && e.hi == hi;
  });
  if (it != _peerRanges.end()) {
    _peerRanges.erase(it);
  }
}

void MeshNode::PeerSnapshot(MeshLink* link, bool reset,
                            const std::unordered_set<uint64_t>& channels,
                            const std::vector<ChannelRange>& ranges) {
  if (reset) {
    RemoveLinkEntries(link);
  }
  for (uint64_t channel : channels) {
    PeerAddChannel(link, channel);
  }
  for (const auto& [lo, hi] : ranges) {
    PeerAddRange(link, lo, hi);
  }
}

void MeshNode::PeerAddPostRemove(uint32_t nodeId, uint32_t owner,
                                 uint64_t sender,
                                 const std::shared_ptr<Datagram>& dg) {
  _bundles[nodeId][{owner, sender}].push_back(dg);
}

void MeshNode::PeerClearPostRemoves(uint32_t nodeId, uint32_t owner,
                                    uint64_t sender) {
  auto it = _bundles.find(nodeId);
  if (it == _bundles.end()) {
    return;
  }
  it->second.erase({owner, sender});
  if (it->second.empty()) {
    _bundles.erase(it);
  }
}

void MeshNode::PeerFired(uint32_t firedBy, uint32_t nodeId) {
  if (_peers.contains(nodeId)) {
    spdlog::get("md")->warn(
        "Peer {} fired post removes for {}, but we can still see it", firedBy,
        nodeId);
    return;
  }
  _fired.insert(nodeId);
  _bundles.erase(nodeId);
}

void MeshNode::PeerVisibility(uint32_t nodeId,
                              std::unordered_set<uint32_t> visible) {
  _peerVisibility[nodeId] = std::move(visible);
}

void MeshNode::LearnPeer(uint32_t nodeId, const std::string& addr) {
  if (nodeId == _nodeId || _peers.contains(nodeId)) {
    return;
  }
  EnsureDial(addr);
}

void MeshNode::CollectLinks(const std::vector<uint64_t>& channels,
                            std::unordered_set<MeshLink*>& links) {
  for (uint64_t channel : channels) {
    if (auto it = _peerChannels.find(channel); it != _peerChannels.end()) {
      links.insert(it->second.begin(), it->second.end());
    }
    for (const auto& entry : _peerRanges) {
      if (channel >= entry.lo && channel <= entry.hi) {
        links.insert(entry.link);
      }
    }
  }
}

void MeshNode::BroadcastAddChannel(uint64_t channel) {
  auto dg = MakeControl(MESH_ADD_CHANNEL);
  dg->AddUint64(channel);
  for (const auto& [nodeId, link] : _peers) {
    link->Send(dg);
  }
}

void MeshNode::BroadcastRemoveChannel(uint64_t channel) {
  auto dg = MakeControl(MESH_REMOVE_CHANNEL);
  dg->AddUint64(channel);
  for (const auto& [nodeId, link] : _peers) {
    link->Send(dg);
  }
}

void MeshNode::BroadcastAddRange(uint64_t lo, uint64_t hi) {
  auto dg = MakeControl(MESH_ADD_RANGE);
  dg->AddUint64(lo);
  dg->AddUint64(hi);
  for (const auto& [nodeId, link] : _peers) {
    link->Send(dg);
  }
}

void MeshNode::BroadcastRemoveRange(uint64_t lo, uint64_t hi) {
  auto dg = MakeControl(MESH_REMOVE_RANGE);
  dg->AddUint64(lo);
  dg->AddUint64(hi);
  for (const auto& [nodeId, link] : _peers) {
    link->Send(dg);
  }
}

void MeshNode::AddLocalPostRemove(uint32_t owner, uint64_t sender,
                                  const std::shared_ptr<Datagram>& dg) {
  _localBundle[{owner, sender}].push_back(dg);

  auto frame = MakeControl(MESH_ADD_POST_REMOVE);
  frame->AddUint32(owner);
  frame->AddUint64(sender);
  frame->AddBlob(dg->GetData(), dg->Size());
  for (const auto& [nodeId, link] : _peers) {
    link->Send(frame);
  }
}

void MeshNode::ClearLocalPostRemoves(uint32_t owner, uint64_t sender) {
  if (!_localBundle.erase({owner, sender})) {
    return;
  }

  auto frame = MakeControl(MESH_CLEAR_POST_REMOVES);
  frame->AddUint32(owner);
  frame->AddUint64(sender);
  for (const auto& [nodeId, link] : _peers) {
    link->Send(frame);
  }
}

bool MeshNode::IsLowestLive() const {
  return _peers.empty() || _nodeId < _peers.begin()->first;
}

bool MeshNode::CorroboratedDown(uint32_t nodeId) const {
  // With no third parties our own dead link is the best evidence there is.
  if (_peers.empty()) {
    return true;
  }
  size_t reporters = 0;
  size_t downVotes = 0;
  for (const auto& [peerId, link] : _peers) {
    auto it = _peerVisibility.find(peerId);
    if (it == _peerVisibility.end()) {
      // No heartbeat from this peer yet, wait for its testimony.
      continue;
    }
    ++reporters;
    if (!it->second.contains(nodeId)) {
      ++downVotes;
    }
  }
  if (reporters == 0) {
    return false;
  }
  return downVotes * 2 > reporters;
}

void MeshNode::MaybeFireBundles() {
  if (!IsLowestLive()) {
    return;
  }

  std::vector<uint32_t> ready;
  for (const auto& [nodeId, bundle] : _bundles) {
    if (_peers.contains(nodeId) || _fired.contains(nodeId)) {
      continue;
    }
    if (CorroboratedDown(nodeId)) {
      ready.push_back(nodeId);
    }
  }
  for (uint32_t nodeId : ready) {
    FireBundle(nodeId, "corroborated death");
  }
}

void MeshNode::FireBundle(uint32_t nodeId, const std::string& reason) {
  auto it = _bundles.find(nodeId);
  size_t count = 0;

  if (it != _bundles.end()) {
    for (const auto& [key, dgs] : it->second) {
      for (const auto& dg : dgs) {
        try {
          MessageDirector::Instance()->RouteDatagram(dg);
          ++count;
        } catch (const DatagramIteratorEOF&) {
          spdlog::get("md")->warn(
              "Dead peer {} had a truncated post remove, dropping", nodeId);
        }
      }
    }
    _bundles.erase(it);
  }

  _fired.insert(nodeId);

  spdlog::get("md")->warn("Fired {} post remove(s) for dead peer {} ({})",
                          count, nodeId, reason);

  if (_bundlesFiredCounter) {
    _bundlesFiredCounter->Increment();
  }
  // If someone lower was alive when this peer died they may have fired
  // already, repeating is safer than leaking.
  if (auto lowestIt = _lowestAtDeath.find(nodeId);
      lowestIt != _lowestAtDeath.end() && lowestIt->second != _nodeId) {
    if (_possibleRefiresCounter) {
      _possibleRefiresCounter->Increment();
    }
  }
  _lowestAtDeath.erase(nodeId);

  auto dg = MakeControl(MESH_POST_REMOVES_FIRED);
  dg->AddUint32(nodeId);
  for (const auto& [peerId, link] : _peers) {
    link->Send(dg);
  }
}

void MeshNode::Tick() {
  uint64_t now = SteadyMs();

  // Sweep dead links first, any frame counts as life, heartbeats just
  // guarantee there's always one.
  std::vector<MeshLink*> dead;
  for (const auto& [ptr, link] : _links) {
    if (link->SocketDown()) {
      // Torn down without a socket event, e.g. a high water disconnect.
      dead.push_back(ptr);
      continue;
    }
    if (!link->Live()) {
      if (now - link->CreatedMs() > kHandshakeTimeoutMs) {
        dead.push_back(ptr);
      }
      continue;
    }
    uint64_t timeoutMs = (static_cast<uint64_t>(_missedHeartbeats) *
                          link->HeartbeatIntervalMs()) +
                         (link->HeartbeatIntervalMs() / 2);
    if (now - link->LastReceivedMs() > timeoutMs) {
      spdlog::get("md")->warn("Mesh peer {} missed {} heartbeats",
                              link->NodeId(), _missedHeartbeats);
      dead.push_back(ptr);
    }
  }
  for (MeshLink* link : dead) {
    OnLinkDown(link);
  }

  // Heartbeat everyone, carrying who we can see for corroboration.
  std::vector<uint32_t> visible;
  visible.reserve(_peers.size());
  for (const auto& [nodeId, link] : _peers) {
    visible.push_back(nodeId);
  }
  for (const auto& [nodeId, link] : _peers) {
    link->SendHeartbeat(visible);

    if (auto it = _peerRttGauges.find(nodeId); it != _peerRttGauges.end()) {
      it->second->Set(link->RttMs());
    }
    if (auto it = _peerQueueGauges.find(nodeId); it != _peerQueueGauges.end()) {
      it->second->Set(static_cast<double>(link->GetQueuedBytes()));
    }
  }

  MaybeFireBundles();
}

void MeshNode::CountControlFrame(uint16_t msgType) {
  if (!_controlFramesFamily) {
    return;
  }
  const char* name;
  switch (msgType) {
    case MESH_HELLO:
      name = "hello";
      break;
    case MESH_PEERS:
      name = "peers";
      break;
    case MESH_SNAPSHOT:
      name = "snapshot";
      break;
    case MESH_ADD_CHANNEL:
    case MESH_REMOVE_CHANNEL:
      name = "channel";
      break;
    case MESH_ADD_RANGE:
    case MESH_REMOVE_RANGE:
      name = "range";
      break;
    case MESH_ADD_POST_REMOVE:
    case MESH_CLEAR_POST_REMOVES:
      name = "post_remove";
      break;
    case MESH_POST_REMOVES_FIRED:
      name = "fired";
      break;
    case MESH_HEARTBEAT:
      name = "heartbeat";
      break;
    default:
      name = "unknown";
  }
  _controlFramesFamily->Add({{"type", name}}).Increment();
}

void MeshNode::InitMetrics() {
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  _peersGauge = &prometheus::BuildGauge()
                     .Name("md_mesh_peers")
                     .Help("Number of live mesh peers")
                     .Register(*registry)
                     .Add({});

  _peerVisibleFamily = &prometheus::BuildGauge()
                            .Name("md_peer_visible")
                            .Help("Whether this instance can see a peer")
                            .Register(*registry);

  _peerRttFamily = &prometheus::BuildGauge()
                        .Name("md_mesh_link_rtt_ms")
                        .Help("Heartbeat round trip time per peer link")
                        .Register(*registry);

  _peerQueueFamily = &prometheus::BuildGauge()
                          .Name("md_mesh_link_queued_bytes")
                          .Help("Outbound bytes queued per peer link")
                          .Register(*registry);

  _controlFramesFamily = &prometheus::BuildCounter()
                              .Name("md_mesh_control_frames_total")
                              .Help("Mesh control frames received by type")
                              .Register(*registry);

  _dialAttemptsCounter = &prometheus::BuildCounter()
                              .Name("md_mesh_dial_attempts_total")
                              .Help("Peer dial attempts")
                              .Register(*registry)
                              .Add({});

  _peerLossesCounter = &prometheus::BuildCounter()
                            .Name("md_mesh_peer_losses_total")
                            .Help("Live peers declared dead")
                            .Register(*registry)
                            .Add({});

  _bundlesFiredCounter = &prometheus::BuildCounter()
                              .Name("md_mesh_bundles_fired_total")
                              .Help("Dead peer post remove bundles fired")
                              .Register(*registry)
                              .Add({});

  _possibleRefiresCounter =
      &prometheus::BuildCounter()
           .Name("md_mesh_possible_refires_total")
           .Help("Bundle fires where the original firer died mid duty")
           .Register(*registry)
           .Add({});
}

void MeshNode::SetPeerVisibleMetric(uint32_t nodeId, bool up) {
  if (!_peerVisibleFamily) {
    return;
  }
  auto it = _peerVisibleGauges.find(nodeId);
  if (it == _peerVisibleGauges.end()) {
    std::string label = std::to_string(nodeId);
    _peerVisibleGauges[nodeId] = &_peerVisibleFamily->Add({{"peer", label}});
    _peerRttGauges[nodeId] = &_peerRttFamily->Add({{"peer", label}});
    _peerQueueGauges[nodeId] = &_peerQueueFamily->Add({{"peer", label}});
    it = _peerVisibleGauges.find(nodeId);
  }
  it->second->Set(up ? 1 : 0);
  if (!up) {
    _peerRttGauges[nodeId]->Set(0);
    _peerQueueGauges[nodeId]->Set(0);
  }
}

}  // namespace Ardos
