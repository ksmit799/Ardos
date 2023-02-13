#ifndef ARDOS_CLIENT_PARTICIPANT_H
#define ARDOS_CLIENT_PARTICIPANT_H

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"
#include "../net/network_client.h"
#include "client_agent.h"

namespace Ardos {

enum AuthState {
  AUTH_STATE_NEW,
  AUTH_STATE_ANONYMOUS,
  AUTH_STATE_ESTABLISHED,
};

class ClientParticipant : public NetworkClient, public ChannelSubscriber {
public:
  ClientParticipant(ClientAgent *clientAgent,
                    const std::shared_ptr<uvw::TCPHandle> &socket);

private:
  void Shutdown() override;
  void Annihilate();

  void HandleDisconnect(uv_errno_t code) override;

  void HandleClientDatagram(const std::shared_ptr<Datagram> &dg) override;
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  void SendDisconnect(const uint16_t &reason, const std::string &message,
                      const bool &security = false);

  void HandlePreHello(DatagramIterator &dgi);
  void HandlePreAuth(DatagramIterator &dgi);
  void HandleAuthenticated(DatagramIterator &dgi);

  ClientAgent *_clientAgent;

  uint64_t _channel;
  AuthState _authState = AUTH_STATE_NEW;
  bool _cleanDisconnect = false;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_PARTICIPANT_H
