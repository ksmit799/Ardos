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

  _socket->on<uvw::DataEvent>(
      [this](const uvw::DataEvent &event, uvw::TCPHandle &) {
        HandleData(event.data, event.length);
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

void MDParticipant::HandleData(const std::unique_ptr<char[]> &data,
                               size_t size) {
  // We can't directly handle datagrams as it's possible that multiple have been
  // buffered together, or we've received a split message.

  // First, check if we have one, complete datagram.
  if (_data_buf.empty() && size >= sizeof(uint16_t)) {
    // Ok, we at least have a size header. Let's check if we have the full
    // datagram.
    uint16_t datagramSize = *reinterpret_cast<uint16_t *>(data.get());
    if (datagramSize == size - sizeof(uint16_t)) {
      // We have a complete datagram, lets handle it.
      auto dg = std::make_shared<Datagram>(
          reinterpret_cast<const uint8_t *>(data.get() + sizeof(uint16_t)),
          datagramSize);
      HandleDatagram(dg);
      return;
    }
  }

  // Hmm, we don't. Let's put it into our buffer.
  _data_buf.insert(_data_buf.end(), data.get(), data.get() + size);
  ProcessBuffer();
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
          reinterpret_cast<const char *>(dg->GetData()));
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error(std::format(
        "[MD] Participant '{}' received a truncated datagram!", _connName));
    Shutdown();
  }
}

void MDParticipant::ProcessBuffer() {
  while (_data_buf.size() > sizeof(uint16_t)) {
    // We have enough data to know the expected length of the datagram.
    uint16_t dataSize = *reinterpret_cast<uint16_t *>(&_data_buf[0]);
    if (_data_buf.size() >= dataSize + sizeof(uint16_t)) {
      // We have a complete datagram!
      auto dg = std::make_shared<Datagram>(
          reinterpret_cast<const uint8_t *>(&_data_buf[sizeof(uint16_t)]),
          dataSize);

      // Remove the datagram data from the buffer.
      _data_buf.erase(_data_buf.begin(),
                      _data_buf.begin() + sizeof(uint16_t) + dataSize);

      HandleDatagram(dg);
    } else {
      return;
    }
  }
}

} // namespace Ardos
