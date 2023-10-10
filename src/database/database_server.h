#ifndef ARDOS_DATABASE_SERVER_H
#define ARDOS_DATABASE_SERVER_H

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <prometheus/counter.h>
#include <prometheus/histogram.h>

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"
#include "../net/message_types.h"

namespace Ardos {

class DatabaseServer : public ChannelSubscriber {
public:
  DatabaseServer();

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  uint32_t AllocateDoId();
  void FreeDoId(const uint32_t &doId);

  void HandleCreate(DatagramIterator &dgi, const uint64_t &sender);
  void HandleCreateDone(const uint64_t &channel, const uint32_t &context,
                        const uint32_t &doId);

  void HandleDelete(DatagramIterator &dgi);

  void HandleGetAll(DatagramIterator &dgi, const uint64_t &sender);
  void HandleGetField(DatagramIterator &dgi, const uint64_t &sender,
                      const bool &multiple);

  void HandleSetField(DatagramIterator &dgi, const bool &multiple);
  void HandleSetFieldEquals(DatagramIterator &dgi, const uint64_t &sender,
                            const bool &multiple);

  void HandleContextFailure(const MessageTypes &type, const uint64_t &channel,
                            const uint32_t &context);

  void InitMetrics();
  void InitFreeChannelsMetric();

  uint32_t _minDoId;
  uint32_t _maxDoId;
  uint64_t _channel;

  mongocxx::instance _instance{}; // N.B: This one and only instance must exist
                                  // for the entirety of the program.
  mongocxx::uri _uri;
  mongocxx::client _conn;
  mongocxx::database _db;

  prometheus::Gauge *_freeChannelsGauge = nullptr;

  enum OperationType {
    CREATE_OBJECT,
    DELETE_OBJECT,
    GET_OBJECT,
    GET_OBJECT_FIELDS,
    SET_OBJECT_FIELDS,
    UPDATE_OBJECT_FIELDS,
  };

  std::unordered_map<OperationType, prometheus::Counter *> _opsCompleted;
  std::unordered_map<OperationType, prometheus::Counter *> _opsFailed;
  std::unordered_map<OperationType, prometheus::Histogram *> _opsCompletionTime;
};

} // namespace Ardos

#endif // ARDOS_DATABASE_SERVER_H
