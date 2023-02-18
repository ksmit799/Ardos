#ifndef ARDOS_INTEREST_OPERATION_H
#define ARDOS_INTEREST_OPERATION_H

#include <cstdint>
#include <memory>
#include <unordered_set>

#include <uvw.hpp>

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

private:
  void HandleInterestTimeout();

  std::shared_ptr<uvw::TimerHandle> _timeout;

  std::unordered_set<uint64_t> _callers;
};

} // namespace Ardos

#endif // ARDOS_INTEREST_OPERATION_H
