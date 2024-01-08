#ifndef ARDOS_MD_PARTICIPANT_H
#define ARDOS_MD_PARTICIPANT_H

#include <memory>

#include <uvw.hpp>

#include "../net/datagram.h"
#include "../net/network_client.h"
#include "channel_subscriber.h"

namespace Ardos {

class MDParticipant final : public NetworkClient, public ChannelSubscriber {
public:
  explicit MDParticipant(const std::shared_ptr<uvw::tcp_handle> &socket);
  ~MDParticipant() override;

private:
  void Shutdown() override;
  void HandleDisconnect(uv_errno_t code) override;
  void HandleClientDatagram(const std::shared_ptr<Datagram> &dg) override;
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  std::string _connName = "Unnamed Participant";
  std::vector<std::shared_ptr<Datagram>> _postRemoves;
};

} // namespace Ardos

#endif // ARDOS_MD_PARTICIPANT_H
