#ifndef ARDOS_MESH_LINK_H
#define ARDOS_MESH_LINK_H

#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <uvw.hpp>
#include <vector>

#include "../net/datagram.h"
#include "../net/network_client.h"

namespace Ardos {

class DatagramIterator;
class MeshNode;

using ChannelRange = std::pair<uint64_t, uint64_t>;

/**
 * One TCP link to a peer Ardos instance.
 *
 * A link starts as a bare connection, becomes live once HELLO's have been
 * exchanged, and is torn down for good the moment it's declared dead.
 * A reconnect is always a brand new link, never a resume.
 */
class MeshLink final : public NetworkClient {
 public:
  friend class MeshNode;

  MeshLink(MeshNode* mesh, const std::shared_ptr<uvw::tcp_handle>& socket,
           bool outbound, std::string dialedAddr);
  ~MeshLink();

  [[nodiscard]] bool Live() const { return _live; }
  [[nodiscard]] bool Outbound() const { return _outbound; }
  [[nodiscard]] uint32_t NodeId() const { return _nodeId; }
  [[nodiscard]] uint64_t Epoch() const { return _epoch; }
  [[nodiscard]] std::string ListenAddr() const { return _listenAddr; }
  [[nodiscard]] std::string DialedAddr() const { return _dialedAddr; }
  [[nodiscard]] uint32_t HeartbeatIntervalMs() const { return _heartbeatMs; }
  [[nodiscard]] uint64_t LastReceivedMs() const { return _lastReceivedMs; }
  [[nodiscard]] uint64_t CreatedMs() const { return _createdMs; }
  [[nodiscard]] double RttMs() const { return _rttMs; }

  // NetworkClient hides these behind protected, the mesh needs them.
  void Send(const std::shared_ptr<Datagram>& dg) { SendDatagram(dg); }
  void Close() { Shutdown(); }
  // True once the socket is gone, e.g. a high water disconnect that
  // fires no socket event, the tick sweeps these out.
  [[nodiscard]] bool SocketDown() const { return Disconnected(); }

  void SendHello();
  void SendHeartbeat(const std::vector<uint32_t>& visible);

 private:
  void HandleDisconnect(uv_errno_t code) override;
  void HandleClientDatagram(const std::shared_ptr<Datagram>& dg) override;
  void HandleControl(const std::shared_ptr<Datagram>& dg);
  void HandleHello(DatagramIterator& dgi);
  void HandleHeartbeat(DatagramIterator& dgi);

  MeshNode* _mesh;
  bool _outbound;
  // Address we dialed, empty on accepted links.
  std::string _dialedAddr;

  bool _live = false;
  // Set once the mesh has cleaned this link out of its tables.
  bool _removed = false;

  uint32_t _nodeId = 0;
  uint64_t _epoch = 0;
  uint32_t _heartbeatMs = 1000;
  // The peers own listen address, learned from its HELLO, used for redial.
  std::string _listenAddr;

  uint64_t _createdMs;
  uint64_t _lastReceivedMs;

  // Heartbeats echo the peers last timestamp back, giving us RTT for free.
  uint64_t _peerTsMs = 0;
  uint64_t _peerTsRecvMs = 0;
  double _rttMs = 0;

  // What this peer subscribes to, mirrored here so a dead link can be
  // swept out of the routing tables by walking its own entries.
  std::unordered_set<uint64_t> _channels;
  std::vector<ChannelRange> _ranges;
  // Shared group memberships this peer advertises, channel to count.
  std::map<uint64_t, uint16_t> _shared;
};

}  // namespace Ardos

#endif  // ARDOS_MESH_LINK_H
