#ifndef ARDOS_CLIENT_PARTICIPANT_H
#define ARDOS_CLIENT_PARTICIPANT_H

#include "../messagedirector/channel_subscriber.h"
#include "../net/network_client.h"
#include "client_agent.h"

namespace Ardos {

class ClientParticipant : public NetworkClient, public ChannelSubscriber {
public:
  ClientParticipant(ClientAgent *clientAgent, const std::shared_ptr<uvw::TCPHandle> &socket);

private:
  void Shutdown() override;
  void HandleDisconnect(uv_errno_t code) override;
  void HandleClientDatagram(const std::shared_ptr<Datagram> &dg) override;
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  ClientAgent *_clientAgent;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_PARTICIPANT_H
