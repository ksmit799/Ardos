#include "md_participant.h"

#include <spdlog/spdlog.h>

#include "../net/datagram_iterator.h"
#include "../net/message_types.h"
#include "message_director.h"

namespace Ardos {

MDParticipant::MDParticipant(const std::shared_ptr<uvw::tcp_handle>& socket)
    : NetworkClient(socket) {
  auto address = GetRemoteAddress();
  spdlog::get("md")->info("Participant connected from {}:{}", address.ip,
                          address.port);

  MessageDirector::Instance()->ParticipantJoined();
}

MDParticipant::~MDParticipant() {
  // Call shutdown just in-case (most likely redundant.)
  Shutdown();

  MessageDirector::Instance()->ParticipantLeft(this);
}

/**
 * Manually disconnect and delete this MD participant.
 */
void MDParticipant::Shutdown() {
  if (_disconnected) {
    return;
  }

  // Kill the network connection.
  NetworkClient::Shutdown();

  spdlog::get("md")->debug("Routing post-remove(s) for '{}'", _connName);

  // Route any post remove datagrams we might have stored. Publish
  // before ChannelSubscriber::Shutdown queues us for deletion. We fired
  // them ourselves, so the copies peers hold get cleared.
  for (const auto& [sender, dgs] : _postRemoves) {
    for (const auto& dg : dgs) {
      try {
        PublishDatagram(dg);
      } catch (const DatagramIteratorEOF& e) {
        spdlog::get("md")->warn(
            "Participant '{}' had a truncated post-remove; dropping: {}",
            _connName, e.what());
      } catch (const DatagramOverflow& e) {
        spdlog::get("md")->warn(
            "Participant '{}' had an oversized post-remove; dropping: {}",
            _connName, e.what());
      }
    }
    MessageDirector::Instance()->ClearPostRemoves(sender);
  }
  _postRemoves.clear();

  // Unsubscribe from all channels and queue ourselves for deletion.
  ChannelSubscriber::Shutdown();
}

/**
 * Handles socket disconnect events.
 * @param code
 */
void MDParticipant::HandleDisconnect(uv_errno_t code) {
  auto address = GetRemoteAddress();

  auto errorEvent = uvw::error_event{static_cast<int>(code)};
  spdlog::get("md")->info("Lost connection from '{}' ({}:{}): {}", _connName,
                          address.ip, address.port, errorEvent.what());

  Shutdown();
}

void MDParticipant::HandleClientDatagram(const std::shared_ptr<Datagram>& dg) {
  DatagramIterator dgi(dg);
  try {
    // Is this a control message?
    uint8_t channels = dgi.GetUint8();
    if (channels == 1 && dgi.GetUint64() == CONTROL_MESSAGE) {
      uint16_t msgType = dgi.GetUint16();
      switch (msgType) {
        case CONTROL_ADD_CHANNEL:
          SubscribeChannel(dgi.GetUint64());
          break;
        case CONTROL_REMOVE_CHANNEL:
          UnsubscribeChannel(dgi.GetUint64());
          break;
        case CONTROL_ADD_RANGE: {
          uint64_t min = dgi.GetUint64();
          uint64_t max = dgi.GetUint64();
          SubscribeRange(min, max);
          break;
        }
        case CONTROL_REMOVE_RANGE: {
          uint64_t min = dgi.GetUint64();
          uint64_t max = dgi.GetUint64();
          UnsubscribeRange(min, max);
          break;
        }
        case CONTROL_ADD_POST_REMOVE: {
          uint64_t sender = dgi.GetUint64();
          auto postRemove = dgi.GetDatagram();
          _postRemoves[sender].push_back(postRemove);
          // Replicate to peers so a survivor can fire this if we crash.
          MessageDirector::Instance()->AddPostRemove(sender, postRemove);
          break;
        }
        case CONTROL_CLEAR_POST_REMOVES: {
          uint64_t sender = dgi.GetUint64();
          _postRemoves.erase(sender);
          MessageDirector::Instance()->ClearPostRemoves(sender);
          break;
        }
        case CONTROL_SET_CON_NAME:
          _connName = dgi.GetString();
          break;
        default:
          spdlog::get("md")->error(
              "Participant '{}' received unknown control message: {}",
              _connName, msgType);
      }

      // We've handled their control message, no need to route through MD.
      return;
    }

    // This wasn't a control message, route it through the message director.
    PublishDatagram(dg);
  } catch (const DatagramIteratorEOF&) {
    spdlog::get("md")->error("Participant '{}' received a truncated datagram!",
                             _connName);
    Shutdown();
  }
}

void MDParticipant::HandleDatagram(const std::shared_ptr<Datagram>& dg) {
  spdlog::get("md")->trace("MDP '{}' forwarding {}B to socket", _connName,
                           dg->Size());
  // Forward messages from the MD to the connected participant.
  SendDatagram(dg);
}

}  // namespace Ardos
