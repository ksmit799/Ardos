#ifndef ARDOS_DATABASE_SERVER_H
#define ARDOS_DATABASE_SERVER_H

#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>

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

  void HandleDelete(DatagramIterator &dgi, const uint64_t &sender);

  void HandleGetAll(DatagramIterator &dgi, const uint64_t &sender);
  void HandleGetField(DatagramIterator &dgi, const uint64_t &sender,
                      const bool &multiple);
  void HandleGetFailure(const MessageTypes &type, const uint64_t &channel,
                        const uint32_t &context);

  void HandleSetField(DatagramIterator &dgi, const uint64_t &sender,
                      const bool &multiple);

  void InitMetrics();

  uint32_t _minDoId;
  uint32_t _maxDoId;
  uint64_t _channel;

  mongocxx::instance _instance{}; // N.B: This one and only instance must exist
                                  // for the entirety of the program.
  mongocxx::uri _uri;
  mongocxx::client _conn;
  mongocxx::database _db;
};

} // namespace Ardos

#endif // ARDOS_DATABASE_SERVER_H
