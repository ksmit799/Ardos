#include "md_participant.h"

#include "../net/datagram_iterator.h"
#include "../net/message_types.h"
#include "../util/logger.h"
#include "message_director.h"

namespace Ardos {

MDParticipant::MDParticipant(const std::shared_ptr<uvw::TCPHandle> &socket)
    : NetworkClient(socket), ChannelSubscriber() {
  auto address = GetRemoteAddress();
  Logger::Info(std::format("[MD] Participant connected from {}:{}", address.ip,
                           address.port));
}

/**
 * Manually disconnect and delete this MD participant.
 */
void MDParticipant::Shutdown() { delete this; }

/**
 * Handles socket disconnect events.
 * @param code
 */
void MDParticipant::HandleDisconnect(uv_errno_t code) {
  auto address = GetRemoteAddress();

  auto errorEvent = uvw::ErrorEvent{(int)code};
  Logger::Info(std::format("[MD] Lost connection from '{}' ({}:{}): {}",
                           _connName, address.ip, address.port,
                           errorEvent.what()));

  Shutdown();
}

void MDParticipant::HandleClientDatagram(const std::shared_ptr<Datagram> &dg) {
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
      case CONTROL_ADD_RANGE:
        break;
      case CONTROL_REMOVE_RANGE:
        break;
      case CONTROL_ADD_POST_REMOVE:
        break;
      case CONTROL_CLEAR_POST_REMOVES:
        break;
      case CONTROL_SET_CON_NAME:
        _connName = dgi.GetString();
        break;
      default:
        Logger::Error(std::format(
            "[MD] Participant '{}' received unknown control message: {}",
            _connName, msgType));
      }

      // We've handled their control message, no need to route through MD.
      return;
    }

    // This wasn't a control message, route it through the message director.
    dgi.Seek(1); // Seek just before channels.
    for (uint8_t i = 0; i < channels; ++i) {
      uint64_t channel = dgi.GetUint64();
      MessageDirector::Instance()->GetGlobalChannel()->publish(
          kGlobalExchange, std::to_string(channel),
          reinterpret_cast<const char *>(dg->GetData()), (size_t)dg->Size());
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error(std::format(
        "[MD] Participant '{}' received a truncated datagram!", _connName));
    Shutdown();
  }
}

void MDParticipant::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  // Forward messages from the MD to the connected participant.
  SendDatagram(dg);
}

} // namespace Ardos
