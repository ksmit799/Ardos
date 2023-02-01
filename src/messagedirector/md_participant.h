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
  void HandleDatagram(const std::shared_ptr<Datagram> &dg);

  std::shared_ptr<uvw::TCPHandle> _socket;
  uvw::Addr _remoteAddress;
  std::string _connName = "MD Participant";
  bool _disconnected = false;
};

} // namespace Ardos

#endif // ARDOS_MD_PARTICIPANT_H
