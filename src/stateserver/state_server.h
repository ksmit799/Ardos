#ifndef ARDOS_STATE_SERVER_H
#define ARDOS_STATE_SERVER_H

#include <memory>

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram.h"
#include "../net/datagram_iterator.h"

namespace Ardos {

class DistributedObject;

class StateServer : public ChannelSubscriber {
public:
  StateServer();

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;
  void HandleGenerate(DatagramIterator &dgi, const bool &other);

  uint64_t _channel;
  std::unordered_map<uint32_t, DistributedObject *> _dist_objs;
};

} // namespace Ardos

#endif // ARDOS_STATE_SERVER_H
