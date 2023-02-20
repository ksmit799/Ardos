#include "interest_operation.h"

#include "../net/message_types.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "client_participant.h"

namespace Ardos {

InterestOperation::InterestOperation(
    ClientParticipant *client, const unsigned long &timeout,
    const uint16_t &interestId, const uint32_t &clientContext,
    const uint32_t &requestContext, const uint32_t &parent,
    const std::unordered_set<uint32_t> &zones, const uint64_t &caller)
    : _client(client), _interestId(interestId), _clientContext(clientContext),
      _requestContext(requestContext) {
  _callers.insert(caller);

  // Interest operations can time out if the state server is taking too long.
  _timeout = g_loop->resource<uvw::TimerHandle>();
  _timeout->on<uvw::TimerEvent>(
      [this](const uvw::TimerEvent &, uvw::TimerHandle &) {
        HandleInterestTimeout();
      });

  _timeout->start(uvw::TimerHandle::Time{timeout}, uvw::TimerHandle::Time{0});
}

InterestOperation::~InterestOperation() {
  // Make sure we've finished and don't have a lingering timeout timer.
  assert(!_timeout);
  assert(_finished);
}

void InterestOperation::HandleInterestTimeout() {
  Logger::Warn(std::format("Interest operation: {}:{} timed out, forcing...",
                           _interestId, _clientContext));
  Finish();
}

void InterestOperation::Finish() {
  // Stop and release the time-out timer.
  if (_timeout) {
    _timeout->stop();
    _timeout->close();
    _timeout.reset();
  }

  for (const auto &dg : _pendingGenerates) {
    DatagramIterator dgi(dg);
    dgi.SeekPayload();
    dgi.Skip(sizeof(uint64_t)); // Skip sender.

    uint16_t msgType = dgi.GetUint16();
    bool withOther =
        (msgType == STATESERVER_OBJECT_ENTER_INTEREST_WITH_REQUIRED_OTHER);

    dgi.Skip(sizeof(uint32_t)); // Skip request context.
    _client->HandleObjectEntrance(dgi, withOther);
  }

  _client->NotifyInterestDone(this);
  _client->HandleInterestDone(_interestId, _clientContext);

  // We need to delete the pending interest operation before we send queued
  // datagrams, so that they're not bounced back to this operation.
  std::vector<std::shared_ptr<Datagram>> dispatch =
      std::move(_pendingDatagrams);

  _client->_pendingInterests.erase(_requestContext);

  // Dispatch other received and queued datagrams.
  for (const auto &dg : dispatch) {
    _client->HandleDatagram(dg);
  }

  _finished = true;

  delete this;
}

bool InterestOperation::IsReady() {
  return _hasTotal && _pendingGenerates.size() >= _total;
}

void InterestOperation::SetExpected(const uint32_t &total) {
  if (!_hasTotal) {
    _total = total;
    _hasTotal = true;
  }
}

void InterestOperation::QueueExpected(const std::shared_ptr<Datagram> &dg) {
  _pendingGenerates.push_back(dg);
}

void InterestOperation::QueueDatagram(const std::shared_ptr<Datagram> &dg) {
  _pendingDatagrams.push_back(dg);
}

} // namespace Ardos
