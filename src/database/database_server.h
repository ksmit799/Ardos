#ifndef ARDOS_DATABASE_SERVER_H
#define ARDOS_DATABASE_SERVER_H

#include <dcField.h>
#include <mongocxx/client.hpp>

#include "../messagedirector/channel_subscriber.h"
#include "../net/datagram_iterator.h"

namespace Ardos {

typedef std::map<const DCField *, std::vector<uint8_t>> FieldMap;

class DatabaseServer : public ChannelSubscriber {
public:
  DatabaseServer();

private:
  void HandleDatagram(const std::shared_ptr<Datagram> &dg) override;

  /**
   * Unpacks fields from an incoming datagram.
   * @param dgi
   * @param fieldCount
   * @param out
   * @param clearFields Whether to unpack field values or set them to
   * default/empty.
   * @return
   */
  bool UnpackFields(DatagramIterator &dgi, const uint16_t &fieldCount,
                    FieldMap &out, const bool &clearFields = false);
  /**
   * A specialized version of UnpackFields that also unpacks 'expected' field
   * values. Used in _IF_EQUALS message handling.
   * @param dgi
   * @param fieldCount
   * @param out
   * @param expectedOut
   * @return
   */
  bool UnpackFields(DatagramIterator &dgi, const uint16_t &fieldCount,
                    FieldMap &out, FieldMap &expectedOut);

  void HandleCreate(DatagramIterator &dgi, const uint64_t &sender);
  void HandleCreateDone(const uint64_t &channel, const uint32_t &context,
                        const uint32_t &doId);

  void HandleDelete(DatagramIterator &dgi, const uint64_t &sender);

  uint64_t _channel;
  mongocxx::uri _uri;
  mongocxx::client _conn;
  mongocxx::database _db;
};

} // namespace Ardos

#endif // ARDOS_DATABASE_SERVER_H
