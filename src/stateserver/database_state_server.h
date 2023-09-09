#ifndef ARDOS_DATABASE_STATE_SERVER_H
#define ARDOS_DATABASE_STATE_SERVER_H

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"
#include "distributed_object.h"
#include "loading_object.h"

namespace Ardos {

class DatabaseStateServer : public ChannelSubscriber {
public:
  DatabaseStateServer();

private:
  void SubscribeRange(const uint32_t &min, const uint32_t &max);

  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  void HandleActivate(DatagramIterator &dgi, const bool &other);

  uint64_t _channel;
  uint64_t _dbChannel;

  std::unordered_map<uint32_t, std::unique_ptr<DistributedObject>> _distObjs;
  std::unordered_map<uint32_t, std::unique_ptr<LoadingObject>> _loadObjs;
};

} // namespace Ardos

#endif // ARDOS_DATABASE_STATE_SERVER_H
