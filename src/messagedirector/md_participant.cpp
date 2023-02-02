#include "md_participant.h"

#include "../net/datagram_iterator.h"
#include "../net/message_types.h"
#include "../util/logger.h"
#include "message_director.h"

namespace Ardos {

MDParticipant::MDParticipant(const std::shared_ptr<uvw::TCPHandle> &socket)
    : _socket(socket) {
  // Configure socket options.
  _socket->noDelay(true);
  _socket->keepAlive(true, uvw::TCPHandle::Time{60});

  _remoteAddress = _socket->peer();

  // Setup event listeners.
  _socket->on<uvw::ErrorEvent>(
      [this](const uvw::ErrorEvent &event, uvw::TCPHandle &) {
        HandleDisconnect((uv_errno_t)event.code());
      });

  _socket->on<uvw::EndEvent>([this](const uvw::EndEvent &, uvw::TCPHandle &) {
    HandleDisconnect(UV_EOF);
  });

  _socket->on<uvw::CloseEvent>(
      [this](const uvw::CloseEvent &, uvw::TCPHandle &) {
        HandleDisconnect(UV_EOF);
      });

  _socket->on<uvw::DataEvent>([this](const uvw::DataEvent &event,
                                     uvw::TCPHandle &) {
    auto dg = std::make_shared<Datagram>(
        reinterpret_cast<const uint8_t *>(event.data.get() + sizeof(uint16_t)),
        event.length);
    HandleDatagram(dg);
  });

  Logger::Info(std::format("[MD] Participant connected from {}:{}",
                           _remoteAddress.ip, _remoteAddress.port));

  _socket->read();
}

MDParticipant::~MDParticipant() {
  _socket->close();
  _socket.reset();
}

/**
 * Manually disconnect and delete this MD participant.
 */
void MDParticipant::Shutdown() {
  _disconnected = true;
  delete this;
}

/**
 * Handles socket disconnect events.
 * @param code
 */
void MDParticipant::HandleDisconnect(uv_errno_t code) {
  if (_disconnected) {
    // We've already handled this participants disconnect.
    return;
  }

  _disconnected = true;

  auto errorEvent = uvw::ErrorEvent{(int)code};
  Logger::Info(std::format("[MD] Lost connection from '{}' ({}:{}): {}",
                           _connName, _remoteAddress.ip, _remoteAddress.port,
                           errorEvent.what()));

  Shutdown();
}

void MDParticipant::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);
  try {
    // Is this a control message?
    uint8_t channels = dgi.GetUint8();
    if (channels == 1 && dgi.GetUint64() == CONTROL_MESSAGE) {
      uint16_t msgType = dgi.GetUint16();
      switch (msgType) {
      case CONTROL_ADD_CHANNEL:
        break;
      case CONTROL_REMOVE_CHANNEL:
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
          reinterpret_cast<const char *>(dg->GetData()));
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error(std::format(
        "[MD] Participant '{}' received a truncated datagram!", _connName));
    Shutdown();
  }
}

} // namespace Ardos
