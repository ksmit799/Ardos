#ifndef ARDOS_MESH_NODE_H
#define ARDOS_MESH_NODE_H

#include <prometheus/counter.h>
#include <prometheus/gauge.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <uvw.hpp>
#include <vector>

#include "../net/datagram.h"
#include "mesh_link.h"

namespace Ardos {

/**
 * The peer mesh backbone.
 *
 * Every Ardos instance connects directly to every other, there is no hub
 * and no relay, a datagram crosses exactly one link. This class owns the
 * links, the peer routing tables, membership, heartbeats and the
 * replicated post remove bundles.
 *
 * An instance with no mesh config never constructs one of these, a
 * cluster of one routes everything in process.
 */
class MeshNode {
 public:
  MeshNode();

  [[nodiscard]] uint32_t GetNodeId() const { return _nodeId; }
  [[nodiscard]] uint64_t GetEpoch() const { return _epoch; }
  [[nodiscard]] size_t GetPeerCount() const { return _peers.size(); }
  [[nodiscard]] std::string GetListenHost() const { return _host; }
  [[nodiscard]] int GetListenPort() const { return _port; }
  [[nodiscard]] uint32_t GetHeartbeatMs() const { return _heartbeatMs; }

  struct PeerInfo {
    uint32_t nodeId;
    std::string addr;
    double rttMs;
  };
  [[nodiscard]] std::vector<PeerInfo> GetPeerInfo() const;

  // Subscription advertising, called when a channel or range gains its
  // first local subscriber or loses its last one.
  void BroadcastAddChannel(uint64_t channel);
  void BroadcastRemoveChannel(uint64_t channel);
  void BroadcastAddRange(uint64_t lo, uint64_t hi);
  void BroadcastRemoveRange(uint64_t lo, uint64_t hi);

  // This instances cleanup bundle, replicated to every peer so a survivor
  // can fire it if we die uncleanly. Keyed by (owner, sender), the owner
  // token scopes entries to one participant connection.
  using BundleKey = std::pair<uint32_t, uint64_t>;
  void AddLocalPostRemove(uint32_t owner, uint64_t sender,
                          const std::shared_ptr<Datagram>& dg);
  void ClearLocalPostRemoves(uint32_t owner, uint64_t sender);

  // Collects every peer link subscribed to any of the given channels.
  void CollectLinks(const std::vector<uint64_t>& channels,
                    std::unordered_set<MeshLink*>& links);

  // Called by links as frames arrive.
  void OnLinkHello(MeshLink* link);
  void OnLinkDown(MeshLink* link);
  void PeerAddChannel(MeshLink* link, uint64_t channel);
  void PeerRemoveChannel(MeshLink* link, uint64_t channel);
  void PeerAddRange(MeshLink* link, uint64_t lo, uint64_t hi);
  void PeerRemoveRange(MeshLink* link, uint64_t lo, uint64_t hi);
  void PeerSnapshot(MeshLink* link, bool reset,
                    const std::unordered_set<uint64_t>& channels,
                    const std::vector<ChannelRange>& ranges);
  void PeerAddPostRemove(uint32_t nodeId, uint32_t owner, uint64_t sender,
                         const std::shared_ptr<Datagram>& dg);
  void PeerClearPostRemoves(uint32_t nodeId, uint32_t owner, uint64_t sender);
  void PeerFired(uint32_t firedBy, uint32_t nodeId);
  void PeerVisibility(uint32_t nodeId, std::unordered_set<uint32_t> visible);
  void LearnPeer(uint32_t nodeId, const std::string& addr);
  void CountControlFrame(uint16_t msgType);

  static std::shared_ptr<Datagram> MakeControl(uint16_t msgType);
  static uint64_t SteadyMs();

 private:
  void Listen();
  void EnsureDial(const std::string& addr);
  void DialNow(const std::string& addr);
  void ScheduleRetry(const std::string& addr);
  void CloseLink(MeshLink* link);
  void RemoveLinkEntries(MeshLink* link);
  void SendPeerState(MeshLink* link);
  void Tick();
  void CheckIsolation();

  [[nodiscard]] bool IsLowestLive() const;
  [[nodiscard]] bool CorroboratedDown(uint32_t nodeId) const;
  void MaybeFireBundles();
  void FireBundle(uint32_t nodeId, const std::string& reason);

  void InitMetrics();
  void SetPeerVisibleMetric(uint32_t nodeId, bool up);

  uint32_t _nodeId = 0;
  uint64_t _epoch = 0;

  std::string _host = "0.0.0.0";
  int _port = 7200;
  uint32_t _heartbeatMs = 1000;
  uint32_t _missedHeartbeats = 3;

  std::shared_ptr<uvw::tcp_handle> _listenHandle;
  std::shared_ptr<uvw::timer_handle> _tickTimer;

  // Every open link, live or still handshaking, owned here.
  std::unordered_map<MeshLink*, std::shared_ptr<MeshLink>> _links;
  // Live peers by node id, ordered so the lowest live id is cheap to find.
  std::map<uint32_t, MeshLink*> _peers;
  // Node ids seen live since boot, used by the isolation check.
  std::unordered_set<uint32_t> _everPeers;

  // Peer routing tables, points in a hash map, ranges in a flat vector,
  // ranges are few, wide and stable so a linear scan wins on simplicity.
  struct RangeEntry {
    uint64_t lo;
    uint64_t hi;
    MeshLink* link;
  };
  std::unordered_map<uint64_t, std::unordered_set<MeshLink*>> _peerChannels;
  std::vector<RangeEntry> _peerRanges;

  // Who each peer says it can see, piggybacked on heartbeats, this is the
  // corroboration table that gates post remove firing.
  std::unordered_map<uint32_t, std::unordered_set<uint32_t>> _peerVisibility;

  // Post remove bundles, ours to replicate out, theirs held in case we
  // have to fire them.
  std::map<BundleKey, std::vector<std::shared_ptr<Datagram>>> _localBundle;
  std::unordered_map<
      uint32_t, std::map<BundleKey, std::vector<std::shared_ptr<Datagram>>>>
      _bundles;
  // Nodes whose bundle has been fired, cleared when they rejoin.
  std::unordered_set<uint32_t> _fired;
  // Lowest live id when each peer died, used to spot possible refires.
  std::unordered_map<uint32_t, uint32_t> _lowestAtDeath;

  // Dial state per address. Seeds retry forever, learned addresses give
  // up after enough failures, a returning peer dials in via its own
  // seeds anyway and gossip re-teaches its current address.
  struct Dial {
    std::string host;
    int port = 0;
    uint32_t backoffMs = 0;
    uint32_t failures = 0;
    bool seed = false;
    bool self = false;
    std::shared_ptr<uvw::timer_handle> retryTimer;
    std::shared_ptr<uvw::tcp_handle> connecting;
  };
  std::unordered_map<std::string, Dial> _dials;

  static constexpr uint32_t kDialBackoffStartMs = 1000;
  static constexpr uint32_t kDialBackoffMaxMs = 15000;
  static constexpr uint32_t kMaxLearnedDialFailures = 10;
  static constexpr uint64_t kHandshakeTimeoutMs = 5000;

  prometheus::Gauge* _peersGauge = nullptr;
  prometheus::Counter* _dialAttemptsCounter = nullptr;
  prometheus::Counter* _peerLossesCounter = nullptr;
  prometheus::Counter* _bundlesFiredCounter = nullptr;
  prometheus::Counter* _possibleRefiresCounter = nullptr;
  prometheus::Family<prometheus::Gauge>* _peerVisibleFamily = nullptr;
  prometheus::Family<prometheus::Gauge>* _peerRttFamily = nullptr;
  prometheus::Family<prometheus::Gauge>* _peerQueueFamily = nullptr;
  prometheus::Family<prometheus::Counter>* _controlFramesFamily = nullptr;
  std::unordered_map<uint32_t, prometheus::Gauge*> _peerVisibleGauges;
  std::unordered_map<uint32_t, prometheus::Gauge*> _peerRttGauges;
  std::unordered_map<uint32_t, prometheus::Gauge*> _peerQueueGauges;
};

}  // namespace Ardos

#endif  // ARDOS_MESH_NODE_H
