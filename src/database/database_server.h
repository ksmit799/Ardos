#ifndef ARDOS_DATABASE_SERVER_H
#define ARDOS_DATABASE_SERVER_H

#include <mongocxx/client.hpp>

#include "../messagedirector/channel_subscriber.h"

namespace Ardos {

class DatabaseServer : public ChannelSubscriber {
public:
  DatabaseServer();

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  uint64_t _channel;
  mongocxx::uri _uri;
  mongocxx::client _conn;
  mongocxx::database _db;
};

} // namespace Ardos

#endif // ARDOS_DATABASE_SERVER_H
