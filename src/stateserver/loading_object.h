#ifndef ARDOS_LOADING_OBJECT_H
#define ARDOS_LOADING_OBJECT_H

#include <dcClass.h>

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"
#include "../util/globals.h"
#include "database_state_server.h"

namespace Ardos {

class LoadingObject : public ChannelSubscriber {
public:
  friend class DatabaseStateServer;

  LoadingObject(DatabaseStateServer *stateServer, const uint32_t &doId,
                const uint32_t &parentId, const uint32_t &zoneId,
                const std::unordered_set<uint32_t> &contexts =
                    std::unordered_set<uint32_t>());
  LoadingObject(DatabaseStateServer *stateServer, const uint32_t &doId,
                const uint32_t &parentId, const uint32_t &zoneId,
                DCClass *dclass, DatagramIterator &dgi,
                const std::unordered_set<uint32_t> &contexts =
                    std::unordered_set<uint32_t>());

  void Start();

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  void Finalize();

  void ReplayDatagrams(DistributedObject *distObj);
  void ForwardDatagrams();

  DatabaseStateServer *_stateServer;

  uint32_t _doId;
  uint32_t _parentId;
  uint32_t _zoneId;

  uint32_t _context;
  std::unordered_set<uint32_t> _validContexts;

  DCClass *_dclass = nullptr;

  bool _isLoaded = false;

  FieldMap _fieldUpdates;
  FieldMap _requiredFields;
  FieldMap _ramFields;

  std::vector<std::shared_ptr<Datagram>> _datagramQueue;

  uvw::timer_handle::time _startTime;
};

} // namespace Ardos

#endif // ARDOS_LOADING_OBJECT_H
