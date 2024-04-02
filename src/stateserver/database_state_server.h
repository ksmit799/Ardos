#ifndef ARDOS_DATABASE_STATE_SERVER_H
#define ARDOS_DATABASE_STATE_SERVER_H

#include <uvw/timer.h>

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"
#include "../util/globals.h"
#include "distributed_object.h"

namespace Ardos {

bool UnpackDBFields(DatagramIterator &dgi, DCClass *dclass, FieldMap &required,
                    FieldMap &ram);

class LoadingObject;

class DatabaseStateServer final : public StateServerImplementation,
                                  public ChannelSubscriber {
public:
  friend class LoadingObject;

  DatabaseStateServer();

  void RemoveDistributedObject(const uint32_t &doId) override;

  void HandleWeb(ws28::Client *client, nlohmann::json &data);

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  void ReceiveObject(DistributedObject *distObj);
  void DiscardLoader(const uint32_t &doId);

  void HandleActivate(DatagramIterator &dgi, const bool &other);
  void HandleDeleteDisk(DatagramIterator &dgi, const uint64_t &sender);
  void HandleSetField(DatagramIterator &dgi, const bool &multiple);

  void HandleGetField(DatagramIterator &dgi, const uint64_t &sender,
                      const bool &multiple);
  void HandleGetFieldResp(DatagramIterator &dgi, const bool &multiple);

  void HandleGetAll(DatagramIterator &dgi, const uint64_t &sender);
  void HandleGetAllResp(DatagramIterator &dgi);

  void HandleGetActivated(DatagramIterator &dgi, const uint64_t &sender);

  void InitMetrics();

  void ReportActivateTime(const uvw::timer_handle::time &startTime);

  uint64_t _dbChannel;

  std::unordered_map<uint32_t, DistributedObject *> _distObjs;
  std::unordered_map<uint32_t, LoadingObject *> _loadObjs;

  // DoId -> Request context
  std::unordered_map<uint32_t, std::unordered_set<uint32_t>> _inactiveLoads;

  std::unordered_map<uint32_t, std::shared_ptr<Datagram>> _contextDatagrams;

  uint32_t _nextContext = 0;

  prometheus::Gauge *_objectsGauge = nullptr;
  prometheus::Gauge *_loadingGauge = nullptr;

  prometheus::Histogram *_objectsSize = nullptr;
  prometheus::Histogram *_activateTime = nullptr;
};

} // namespace Ardos

#endif // ARDOS_DATABASE_STATE_SERVER_H
