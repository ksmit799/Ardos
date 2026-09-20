#include "mesh_link.h"

#include <spdlog/spdlog.h>

#include <utility>

#include "../net/datagram_iterator.h"
#include "../net/message_types.h"
#include "mesh_node.h"
#include "message_director.h"

namespace Ardos {

MeshLink::MeshLink(MeshNode* mesh,
                   const std::shared_ptr<uvw::tcp_handle>& socket,
                   bool outbound, std::string dialedAddr)
    : NetworkClient(socket),
      _mesh(mesh),
      _outbound(outbound),
      _dialedAddr(std::move(dialedAddr)) {
  _createdMs = MeshNode::SteadyMs();
  _lastReceivedMs = _createdMs;

  auto address = GetRemoteAddress();
  spdlog::get("md")->debug("Mesh link {} {}:{}",
                           _outbound ? "dialed" : "accepted", address.ip,
                           address.port);
}

MeshLink::~MeshLink() { Shutdown(); }

void MeshLink::SendHello() {
  auto dg = MeshNode::MakeControl(MESH_HELLO);
  dg->AddUint32(_mesh->GetNodeId());
  dg->AddUint64(_mesh->GetEpoch());
  dg->AddUint16(MESH_PROTO_VERSION);
  dg->AddUint32(_mesh->GetHeartbeatMs());
  dg->AddUint16(static_cast<uint16_t>(_mesh->GetListenPort()));
  SendDatagram(dg);
}

void MeshLink::SendHeartbeat(const std::vector<uint32_t>& visible) {
  uint64_t now = MeshNode::SteadyMs();

  auto dg = MeshNode::MakeControl(MESH_HEARTBEAT);
  dg->AddUint64(_mesh->GetEpoch());
  dg->AddUint64(now);
  // Echo the peers last timestamp and how long we held it, they subtract
  // both from their clock to get the link RTT.
  dg->AddUint64(_peerTsMs);
  dg->AddUint32(_peerTsMs ? static_cast<uint32_t>(now - _peerTsRecvMs) : 0);
  dg->AddUint16(static_cast<uint16_t>(visible.size()));
  for (uint32_t id : visible) {
    dg->AddUint32(id);
  }
  SendDatagram(dg);
}

void MeshLink::HandleDisconnect(uv_errno_t code) {
  auto address = GetRemoteAddress();
  auto errorEvent = uvw::error_event{static_cast<int>(code)};
  spdlog::get("md")->debug("Mesh link to {}:{} closed: {}", address.ip,
                           address.port, errorEvent.what());

  _mesh->OnLinkDown(this);
}

void MeshLink::HandleClientDatagram(const std::shared_ptr<Datagram>& dg) {
  _lastReceivedMs = MeshNode::SteadyMs();

  try {
    // Routed datagrams always start with a non zero channel count, mesh
    // control frames start with a zero byte.
    DatagramIterator dgi(dg);
    if (dgi.GetUint8() != MESH_CONTROL_HEADER) {
      if (!_live) {
        // No routing before the handshake completes.
        return;
      }
      MessageDirector::Instance()->RouteLocally(dg);
      return;
    }

    HandleControl(dg);
  } catch (const DatagramIteratorEOF&) {
    auto address = GetRemoteAddress();
    spdlog::get("md")->error("Mesh link {}:{} sent a truncated frame",
                             address.ip, address.port);
    _mesh->OnLinkDown(this);
  }
}

void MeshLink::HandleControl(const std::shared_ptr<Datagram>& dg) {
  DatagramIterator dgi(dg, sizeof(uint8_t));
  uint16_t msgType = dgi.GetUint16();

  _mesh->CountControlFrame(msgType);

  if (msgType == MESH_HELLO) {
    HandleHello(dgi);
    return;
  }

  if (!_live) {
    // Everything except HELLO waits for the handshake.
    return;
  }

  switch (msgType) {
    case MESH_HEARTBEAT:
      HandleHeartbeat(dgi);
      break;
    case MESH_ADD_CHANNEL:
      _mesh->PeerAddChannel(this, dgi.GetUint64());
      break;
    case MESH_REMOVE_CHANNEL:
      _mesh->PeerRemoveChannel(this, dgi.GetUint64());
      break;
    case MESH_ADD_RANGE: {
      uint64_t lo = dgi.GetUint64();
      uint64_t hi = dgi.GetUint64();
      _mesh->PeerAddRange(this, lo, hi);
      break;
    }
    case MESH_REMOVE_RANGE: {
      uint64_t lo = dgi.GetUint64();
      uint64_t hi = dgi.GetUint64();
      _mesh->PeerRemoveRange(this, lo, hi);
      break;
    }
    case MESH_SNAPSHOT: {
      bool reset = dgi.GetUint8() != 0;
      std::unordered_set<uint64_t> channels;
      uint32_t channelCount = dgi.GetUint32();
      for (uint32_t i = 0; i < channelCount; ++i) {
        channels.insert(dgi.GetUint64());
      }
      std::vector<ChannelRange> ranges;
      uint32_t rangeCount = dgi.GetUint32();
      for (uint32_t i = 0; i < rangeCount; ++i) {
        uint64_t lo = dgi.GetUint64();
        uint64_t hi = dgi.GetUint64();
        ranges.emplace_back(lo, hi);
      }
      _mesh->PeerSnapshot(this, reset, channels, ranges);
      break;
    }
    case MESH_ADD_POST_REMOVE: {
      uint32_t owner = dgi.GetUint32();
      uint64_t sender = dgi.GetUint64();
      _mesh->PeerAddPostRemove(_nodeId, owner, sender, dgi.GetDatagram());
      break;
    }
    case MESH_CLEAR_POST_REMOVES: {
      uint32_t owner = dgi.GetUint32();
      uint64_t sender = dgi.GetUint64();
      _mesh->PeerClearPostRemoves(_nodeId, owner, sender);
      break;
    }
    case MESH_POST_REMOVES_FIRED:
      _mesh->PeerFired(_nodeId, dgi.GetUint32());
      break;
    case MESH_PEERS: {
      uint16_t count = dgi.GetUint16();
      for (uint16_t i = 0; i < count; ++i) {
        uint32_t nodeId = dgi.GetUint32();
        std::string addr = dgi.GetString();
        _mesh->LearnPeer(nodeId, addr);
      }
      break;
    }
    default:
      spdlog::get("md")->error("Mesh link {} sent unknown control frame: {}",
                               _nodeId, msgType);
  }
}

void MeshLink::HandleHello(DatagramIterator& dgi) {
  if (_live) {
    spdlog::get("md")->error("Mesh link {} sent a second HELLO", _nodeId);
    _mesh->OnLinkDown(this);
    return;
  }

  uint32_t nodeId = dgi.GetUint32();
  uint64_t epoch = dgi.GetUint64();
  uint16_t protoVer = dgi.GetUint16();
  uint32_t heartbeatMs = dgi.GetUint32();
  uint16_t listenPort = dgi.GetUint16();

  if (protoVer != MESH_PROTO_VERSION) {
    auto address = GetRemoteAddress();
    spdlog::get("md")->error(
        "Mesh link {}:{} speaks protocol version {}, we speak {}", address.ip,
        address.port, protoVer, MESH_PROTO_VERSION);
    _mesh->OnLinkDown(this);
    return;
  }

  _nodeId = nodeId;
  _epoch = epoch;
  _heartbeatMs = heartbeatMs ? heartbeatMs : 1000;
  // The peers listen host is just where it dialed us from, combined with
  // the port it says it listens on this is a dialable address.
  _listenAddr = GetRemoteAddress().ip + ":" + std::to_string(listenPort);
  _live = true;

  // The dialer sent its HELLO at connect, the acceptor replies here.
  if (!_outbound) {
    SendHello();
  }

  _mesh->OnLinkHello(this);
}

void MeshLink::HandleHeartbeat(DatagramIterator& dgi) {
  uint64_t epoch = dgi.GetUint64();
  uint64_t ts = dgi.GetUint64();
  uint64_t echoTs = dgi.GetUint64();
  uint32_t echoDelayMs = dgi.GetUint32();

  if (epoch != _epoch) {
    // A frame from a different incarnation, this link is stale.
    spdlog::get("md")->warn("Mesh peer {} heartbeat epoch mismatch", _nodeId);
    _mesh->OnLinkDown(this);
    return;
  }

  uint64_t now = MeshNode::SteadyMs();
  _peerTsMs = ts;
  _peerTsRecvMs = now;
  if (echoTs) {
    uint64_t elapsed = now - echoTs;
    _rttMs = elapsed > echoDelayMs ? static_cast<double>(elapsed - echoDelayMs)
                                   : 0.0;
  }

  std::unordered_set<uint32_t> visible;
  uint16_t count = dgi.GetUint16();
  for (uint16_t i = 0; i < count; ++i) {
    visible.insert(dgi.GetUint32());
  }
  _mesh->PeerVisibility(_nodeId, std::move(visible));
}

}  // namespace Ardos
