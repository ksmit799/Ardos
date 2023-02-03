#ifndef ARDOS_MD_PARTICIPANT_H
#define ARDOS_MD_PARTICIPANT_H

#include <memory>

#include <uvw.hpp>

#include "../net/datagram.h"

namespace Ardos {

class MDParticipant {
public:
  explicit MDParticipant(const std::shared_ptr<uvw::TCPHandle> &socket);
  ~MDParticipant();

private:
  void Shutdown();
  void HandleDisconnect(uv_errno_t code);
  void HandleData(const std::unique_ptr<char[]> &data, size_t size);
  void HandleDatagram(const std::shared_ptr<Datagram> &dg);
  void ProcessBuffer();

  std::shared_ptr<uvw::TCPHandle> _socket;
  uvw::Addr _remoteAddress;
  std::string _connName = "Unnamed Participant";
  bool _disconnected = false;
  std::vector<uint8_t> _data_buf;
};

} // namespace Ardos

#endif // ARDOS_MD_PARTICIPANT_H
