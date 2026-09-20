#ifndef ARDOS_MESSAGE_DIRECTOR_H
#define ARDOS_MESSAGE_DIRECTOR_H

#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <ws28/Client.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <uvw.hpp>

namespace Ardos {

class ChannelSubscriber;
class Datagram;
class MDParticipant;
class MeshNode;

class StateServer;
class ClientAgent;
class DatabaseServer;
class DatabaseStateServer;
class WebPanel;

class MessageDirector {
 public:
  static MessageDirector* Instance();

  // Starts configured roles and opens the participant listen socket.
  // Roles reach back into Instance(), so they can't start in the ctor.
  void StartRoles();

  void AddSubscriber(std::shared_ptr<ChannelSubscriber> subscriber);
  // Raw-pointer overload: callable from ChannelSubscriber::~ at a point
  // where shared_from_this() is no longer valid. Walks _subscribers and
  // erases the matching shared_ptr by .get() identity. O(N) but only hit
  // on subscriber teardown.
  void RemoveSubscriber(ChannelSubscriber* subscriber);

  // Routes a locally published datagram to in process subscribers and to
  // every subscribed peer, one frame per link.
  void RouteDatagram(const std::shared_ptr<Datagram>& dg);
  // Routes a datagram received from a peer, local delivery only, an
  // instance never relays a peers datagram onward.
  void RouteLocally(const std::shared_ptr<Datagram>& dg);

  // Subscription advertising, forwarded to the mesh, no-ops standalone.
  void BroadcastAddChannel(uint64_t channel);
  void BroadcastRemoveChannel(uint64_t channel);
  void BroadcastAddRange(uint64_t lo, uint64_t hi);
  void BroadcastRemoveRange(uint64_t lo, uint64_t hi);

  // This instances post remove bundle, replicated to peers via the mesh.
  // Entries are keyed by (owner, sender), the owner token scopes them to
  // one participant connection, so a stale connections clear can't wipe
  // a newer connections entries under the same sender channel.
  [[nodiscard]] uint32_t AllocPostRemoveOwner() { return ++_postRemoveOwner; }
  void AddPostRemove(uint32_t owner, uint64_t sender,
                     const std::shared_ptr<Datagram>& dg);
  void ClearPostRemoves(uint32_t owner, uint64_t sender);

  [[nodiscard]] MeshNode* GetMesh() const { return _mesh; }

  void ParticipantJoined();
  void ParticipantLeft(MDParticipant* participant);

  void HandleWeb(ws28::Client* client, nlohmann::json& data);

  [[nodiscard]] StateServer* GetStateServer() const {
    return _stateServer.get();
  }
  [[nodiscard]] ClientAgent* GetClientAgent() const {
    return _clientAgent.get();
  }
  [[nodiscard]] DatabaseServer* GetDbServer() const { return _db.get(); }
  [[nodiscard]] std::shared_ptr<DatabaseServer> GetDbServerShared() const {
    return _db;
  }
  [[nodiscard]] DatabaseStateServer* GetDbStateServer() const {
    return _dbss.get();
  }

 private:
  MessageDirector();

  void Route(const std::shared_ptr<Datagram>& dg, bool toPeers);

  void InitMetrics();

  static MessageDirector* _instance;

  // The mesh backbone, null when no mesh is configured, a cluster of one
  // routes everything in process. Owned for the process lifetime.
  MeshNode* _mesh = nullptr;

  // Singletons that also inherit ChannelSubscriber are shared_ptr so
  // they can live in _subscribers; ClientAgent doesn't and stays unique.
  std::shared_ptr<StateServer> _stateServer;
  std::unique_ptr<ClientAgent> _clientAgent;
  std::shared_ptr<DatabaseServer> _db;
  std::shared_ptr<DatabaseStateServer> _dbss;
  std::unique_ptr<WebPanel> _webPanel;

  std::unordered_set<std::shared_ptr<ChannelSubscriber>> _subscribers;
  std::unordered_set<MDParticipant*> _participants;

  uint32_t _postRemoveOwner = 0;

  std::shared_ptr<uvw::tcp_handle> _listenHandle;

  // Listen info.
  std::string _host = "127.0.0.1";
  int _port = 7100;

  prometheus::Counter* _datagramsObservedCounter = nullptr;
  prometheus::Counter* _datagramsProcessedCounter = nullptr;
  prometheus::Histogram* _datagramsSizeHistogram = nullptr;
  prometheus::Histogram* _fanoutLinksHistogram = nullptr;
  prometheus::Counter* _localDeliveriesCounter = nullptr;
  prometheus::Counter* _remoteSendsCounter = nullptr;
  prometheus::Gauge* _subscribersGauge = nullptr;
  prometheus::Gauge* _participantsGauge = nullptr;
};

}  // namespace Ardos

#endif  // ARDOS_MESSAGE_DIRECTOR_H
