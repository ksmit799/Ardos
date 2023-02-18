#include "interest_operation.h"

#include "../util/globals.h"
#include "client_participant.h"

namespace Ardos {

InterestOperation::InterestOperation(
    ClientParticipant *client, const unsigned long &timeout,
    const uint16_t &interestId, const uint32_t &clientContext,
    const uint32_t &requestContext, const uint32_t &parent,
    const std::unordered_set<uint32_t> &zones, const uint64_t &caller) {
  _callers.insert(caller);

  // Interest operations can time out if the state server is taking too long.
  _timeout = g_loop->resource<uvw::TimerHandle>();
  _timeout->on<uvw::TimerEvent>(
      [this](const uvw::TimerEvent &, uvw::TimerHandle &) {
        HandleInterestTimeout();
      });

  _timeout->start(uvw::TimerHandle::Time{timeout}, uvw::TimerHandle::Time{0});
}

InterestOperation::~InterestOperation() {}

void InterestOperation::HandleInterestTimeout() {

}

} // namespace Ardos
