#ifndef ARDOS_DISTRIBUTED_OBJECT_H
#define ARDOS_DISTRIBUTED_OBJECT_H

#include <dcClass.h>

#include "../net/message_types.h"
#include "state_server.h"

namespace Ardos {

class DistributedObject : public ChannelSubscriber {
public:
  DistributedObject(StateServer *stateServer, const uint32_t &doId,
                    const uint32_t &parentId, const uint32_t &zoneId,
                    DCClass *dclass, DatagramIterator &dgi, const bool &other);

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  void HandleLocationChange(const uint32_t &newParent, const uint32_t &newZone,
                            const uint64_t &sender);
  void WakeChildren();
  void SendLocationEntry(const uint64_t &location);

  StateServer *_stateServer;
  uint32_t _doId;
  uint32_t _parentId;
  uint32_t _zoneId;
  DCClass *_dclass;

  std::unordered_map<const DCField *, std::vector<uint8_t>> _requiredFields;
  std::map<const DCField *, std::vector<uint8_t>> _ramFields;

  uint64_t _aiChannel = INVALID_CHANNEL;
  uint64_t _ownerChannel = INVALID_CHANNEL;
  uint32_t _nextContext = 0;
  bool _aiExplicitlySet = false;
  bool _parentSynchronized = false;
};

} // namespace Ardos

#endif // ARDOS_DISTRIBUTED_OBJECT_H
