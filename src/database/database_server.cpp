#include "database_server.h"

#include <format>

#include <bsoncxx/json.hpp>

#include "../net/datagram_iterator.h"
#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/logger.h"

namespace Ardos {

DatabaseServer::DatabaseServer() : ChannelSubscriber() {
  Logger::Info("Starting Database Server component...");

  // Database Server configuration.
  auto config = Config::Instance()->GetNode("database-server");

  if (!config["channel"]) {
    Logger::Error("[DB] Missing or invalid channel!");
    exit(1);
  }

  if (!config["mongodb-uri"]) {
    Logger::Error("[DB] Missing or invalid MongoDB URI!");
    exit(1);
  }

  try {
    // Make a connection to MongoDB.
    _uri = mongocxx::uri{config["mongodb-uri"].as<std::string>()};
    _conn = mongocxx::client{_uri};
    _db = _conn[_uri.database()];

    // Ping the DB to make sure we've made a successful connection.
    const auto pingCommand = bsoncxx::builder::basic::make_document(
        bsoncxx::builder::basic::kvp("ping", 1));
    _db.run_command(pingCommand.view());
  } catch (const std::exception &e) {
    Logger::Error(
        std::format("[DB] Failed to connect to MongoDB: {}", e.what()));
    exit(1);
  }

  // Start listening to our channel.
  _channel = config["channel"].as<uint64_t>();
  SubscribeChannel(_channel);
  SubscribeChannel(BCHAN_DBSERVERS);

  Logger::Info(std::format("[DB] Connected to MongoDB: {}", _uri.to_string()));
}

void DatabaseServer::HandleDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  // Skip MD routing headers.
  dgi.SeekPayload();

  try {
    uint64_t sender = dgi.GetUint64();
    uint16_t msgType = dgi.GetUint16();
    switch (msgType) {
    case DBSERVER_CREATE_OBJECT:
      break;
    case DBSERVER_OBJECT_DELETE:
      break;
    case DBSERVER_OBJECT_GET_ALL:
    case DBSERVER_OBJECT_GET_FIELD:
    case DBSERVER_OBJECT_GET_FIELDS: {
      break;
    }
    case DBSERVER_OBJECT_SET_FIELD:
    case DBSERVER_OBJECT_SET_FIELDS:
    case DBSERVER_OBJECT_DELETE_FIELD:
    case DBSERVER_OBJECT_DELETE_FIELDS:
      break;
    case DBSERVER_OBJECT_SET_FIELD_IF_EMPTY:
    case DBSERVER_OBJECT_SET_FIELD_IF_EQUALS:
    case DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS:
      break;
    default:
      // Hopefully we managed to unpack the sender...
      Logger::Warn(
          std::format("[DB] Received unknown message: {} from sender: {}",
                      msgType, sender));
    }
  } catch (const DatagramIteratorEOF &) {
    Logger::Error("[DB] Received a truncated datagram!");
  }
}

} // namespace Ardos
