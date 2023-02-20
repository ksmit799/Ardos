#ifndef ARDOS_INTEREST_OPERATION_H
#define ARDOS_INTEREST_OPERATION_H

#include <cstdint>
#include <memory>
#include <unordered_set>

#include <uvw.hpp>

#include "../net/datagram.h"

namespace Ardos {

class ClientParticipant;

class InterestOperation {
public:
  InterestOperation(ClientParticipant *client, const unsigned long &timeout,
                    const uint16_t &interestId, const uint32_t &clientContext,
                    const uint32_t &requestContext, const uint32_t &parent,
                    const std::unordered_set<uint32_t> &zones,
                    const uint64_t &caller);
  ~InterestOperation();

  void Finish();

  friend class ClientParticipant;

private:
  void HandleInterestTimeout();

  bool IsReady();
  void SetExpected(const uint32_t &total);
  void QueueExpected(const std::shared_ptr<Datagram> &dg);
  void QueueDatagram(const std::shared_ptr<Datagram> &dg);

  ClientParticipant *_client;
  uint16_t _interestId;
  uint32_t _clientContext;
  uint32_t _requestContext;
  std::shared_ptr<uvw::TimerHandle> _timeout;
  bool _hasTotal = false;
  uint32_t _total = 0;
  bool _finished = false;

  std::unordered_set<uint64_t> _callers;

  std::vector<std::shared_ptr<Datagram>> _pendingGenerates;
  std::vector<std::shared_ptr<Datagram>> _pendingDatagrams;
};

} // namespace Ardos

#endif // ARDOS_INTEREST_OPERATION_H
