#ifndef ARDOS_DISTRIBUTED_OBJECT_H
#define ARDOS_DISTRIBUTED_OBJECT_H

#include <dcClass.h>

#include "state_server.h"

namespace Ardos {

class DistributedObject : public ChannelSubscriber {
public:
  DistributedObject(StateServer *stateServer, const uint32_t &doId,
                    const uint32_t &parentId, const uint32_t &zoneId,
                    DCClass *dclass, DatagramIterator &dgi, const bool &other);

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  StateServer *_stateServer;
  uint32_t _doId;
  uint32_t _parentId;
  uint32_t _zoneId;
  DCClass *_dclass;

  std::unordered_map<const DCField *, std::vector<uint8_t>> _required_fields;
  std::map<const DCField *, std::vector<uint8_t>> _ram_fields;

  uint64_t _aiChannel;
};

} // namespace Ardos

#endif // ARDOS_DISTRIBUTED_OBJECT_H
