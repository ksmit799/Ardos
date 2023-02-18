#ifndef ARDOS_CLIENT_PARTICIPANT_H
#define ARDOS_CLIENT_PARTICIPANT_H

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"
#include "../net/network_client.h"
#include "client_agent.h"
#include "interest_operation.h"

namespace Ardos {

enum AuthState {
  AUTH_STATE_NEW,
  AUTH_STATE_ANONYMOUS,
  AUTH_STATE_ESTABLISHED,
};

struct DeclaredObject {
  uint32_t doId;
  DCClass *dcc;
};

struct OwnedObject : DeclaredObject {
  uint32_t parent;
  uint32_t zone;
};

struct VisibleObject : DeclaredObject {
  uint32_t parent;
  uint32_t zone;
};

struct Interest {
  uint16_t id;
  uint32_t parent;
  std::unordered_set<uint32_t> zones;
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

  void HandleClientHeartbeat();
  void HandleHeartbeatTimeout();
  void HandleAuthTimeout();

  void HandlePreHello(DatagramIterator &dgi);
  void HandlePreAuth(DatagramIterator &dgi);
  void HandleAuthenticated(DatagramIterator &dgi);

#ifdef ARDOS_USE_LEGACY_CLIENT
  void HandleLoginLegacy(DatagramIterator &dgi);
#endif

  DCClass *LookupObject(const uint32_t &doId);

  void HandleClientObjectUpdateField(DatagramIterator &dgi);
  void HandleClientObjectLocation(DatagramIterator &dgi);
  void HandleClientAddInterest(DatagramIterator &dgi, const bool &multiple);
  void HandleClientRemoveInterest(DatagramIterator &dgi);

  void BuildInterest(DatagramIterator &dgi, const bool &multiple,
                     Interest &out);
  void AddInterest(Interest &i, const uint32_t &context,
                   const uint64_t &caller = 0);

  std::vector<Interest> LookupInterests(const uint32_t &parentId,
                                        const uint32_t &zoneId);

  void NotifyInterestDone(const uint16_t &interestId, const uint64_t &caller);
  void HandleInterestDone(const uint16_t &interestId, const uint32_t &context);

  void CloseZones(const uint32_t &parent,
                  const std::unordered_set<uint32_t> &killedZones);

  void HandleRemoveObject(const uint32_t &doId);

  ClientAgent *_clientAgent;

  uint64_t _channel;

  std::shared_ptr<uvw::TimerHandle> _heartbeatTimer;
  std::shared_ptr<uvw::TimerHandle> _authTimer;

  AuthState _authState = AUTH_STATE_NEW;
  bool _cleanDisconnect = false;

  // A list of all objects visible through open interests.
  std::unordered_set<uint32_t> _seenObjects;
  // A list of all objects that were once visible, but are no longer.
  std::unordered_set<uint32_t> _historicalObjects;
  // A list of objects that's lifetime is bound to this clients' session.
  std::unordered_set<uint32_t> _sessionObjects;
  // A map of objects that this client has ownership of.
  std::unordered_map<uint32_t, OwnedObject> _ownedObjects;
  // A map of all currently visible objects to their data.
  std::unordered_map<uint32_t, VisibleObject> _visibleObjects;
  // A map of all declared objects to their data.
  std::unordered_map<uint32_t, DeclaredObject> _declaredObjects;

  // A map of DoId's to fields marked explicitly send-able.
  std::unordered_map<uint32_t, std::unordered_set<uint16_t>> _fieldsSendable;

  // Context ID for handling interest responses from the state server.
  uint32_t _nextContext = 0;
  // A map of interest id's to interest handles.
  std::unordered_map<uint16_t, Interest> _interests;
  // A map of interest contexts to their in-progress operations.
  std::unordered_map<uint32_t, InterestOperation*> _pendingInterests;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_PARTICIPANT_H
