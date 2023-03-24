#include "database_server.h"

#include <format>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <dcClass.h>

#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "database_utils.h"

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
      HandleCreate(dgi, sender);
      break;
    case DBSERVER_OBJECT_DELETE:
      HandleDelete(dgi, sender);
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

bool DatabaseServer::UnpackFields(DatagramIterator &dgi,
                                  const uint16_t &fieldCount, FieldMap &out,
                                  const bool &clearFields) {
  for (uint16_t i = 0; i < fieldCount; ++i) {
    uint16_t fieldId = dgi.GetUint16();
    auto field = g_dc_file->get_field_by_index(fieldId);
    if (!field) {
      Logger::Error(std::format("[DB] Attempted to unpack invalid field ID: {}",
                                fieldId));
      return false;
    }

    // Make sure the field we're unpacking is marked 'DB'.
    if (field->is_db()) {
      try {
        if (!clearFields) {
          // We're not clearing the fields sent, so get the value.
          dgi.UnpackField(field, out[field]);
        } else if (field->has_default_value()) {
          // We're clearing this field and it has a default value.
          // Set it to that.
          out[field] = field->get_default_value();
        } else {
          // We're clearing this field and it does not have a default value.
          // Set it to a blank vector.
          out[field] = std::vector<uint8_t>();
        }
      } catch (const DatagramIteratorEOF &) {
        Logger::Error(std::format(
            "[DB] Received truncated field in create/modify request: {}",
            field->get_name()));
        return false;
      }
    } else {
      // Oops, we got a non-db field.
      Logger::Error(
          std::format("[DB] Got non-db field in create/modify request: {}",
                      field->get_name()));

      // Don't read-in a non-db field.
      if (!clearFields) {
        dgi.SkipField(field);
      }
    }
  }

  return true;
}

bool DatabaseServer::UnpackFields(DatagramIterator &dgi,
                                  const uint16_t &fieldCount, FieldMap &out,
                                  FieldMap &expectedOut) {
  for (uint16_t i = 0; i < fieldCount; ++i) {
    uint16_t fieldId = dgi.GetUint16();
    auto field = g_dc_file->get_field_by_index(fieldId);
    if (!field) {
      Logger::Error(std::format("[DB] Attempted to unpack invalid field ID: {}",
                                fieldId));
      return false;
    }

    // Make sure the field we're unpacking is marked 'DB'.
    if (field->is_db()) {
      try {
        // Unpack the expected field value.
        dgi.UnpackField(field, expectedOut[field]);
        // Unpack the updated field value.
        dgi.UnpackField(field, out[field]);
      } catch (const DatagramIteratorEOF &) {
        Logger::Error(
            std::format("[DB] Received truncated field in modify request: {}",
                        field->get_name()));
        return false;
      }
    } else {
      // Oops, we got a non-db field.
      Logger::Error(std::format("[DB] Got non-db field in modify request: {}",
                                field->get_name()));

      // We need valid db fields for _IF_EQUALS updates.
      return false;
    }
  }

  return true;
}

void DatabaseServer::HandleCreate(DatagramIterator &dgi,
                                  const uint64_t &sender) {
  uint32_t context = dgi.GetUint32();

  uint16_t dcId = dgi.GetUint16();
  uint16_t fieldCount = dgi.GetUint16();

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class(dcId);
  if (!dcClass) {
    Logger::Error(std::format(
        "[DB] Received create for unknown distributed class: {}", dcId));
    HandleCreateDone(sender, context, INVALID_DO_ID);
    return;
  }

  // Unpack the fields we've received in the create message.
  FieldMap objectFields;
  if (!UnpackFields(dgi, fieldCount, objectFields)) {
    HandleCreateDone(sender, context, INVALID_DO_ID);
    return;
  }

  // Make sure that all present fields actually belong to the distributed class.
  bool errors = false;
  for (const auto &field : objectFields) {
    if (!dcClass->get_field_by_index(field.first->get_number())) {
      // We don't immediately break out here in case we have multiple
      // non-belonging fields.
      Logger::Warn(std::format(
          "[DB] Attempted to create object: {} with non-belonging field: {}",
          dcClass->get_name(), field.first->get_name()));
      errors = true;
    }
  }

  if (errors) {
    HandleCreateDone(sender, context, INVALID_DO_ID);
    return;
  }

  // Set all non-present fields to their default values.
  for (int i = 0; i < dcClass->get_num_inherited_fields(); ++i) {
    auto field = dcClass->get_inherited_field(i);
    if (field->is_db() && field->has_default_value() &&
        !objectFields.contains(field)) {
      objectFields[field] = field->get_default_value();
    }
  }

  auto builder = bsoncxx::builder::stream::document{};
  try {
    for (const auto &field : objectFields) {
      auto dg = std::make_shared<Datagram>();
      dg->AddData(field.second);
      DatagramIterator fieldDgi(dg);
      DatabaseUtils::DCToBson(builder << field.first->get_name(),
                              field.first->get_pack_type(), fieldDgi);
    }
  } catch (const std::exception &) {
  }
  auto fields = builder << bsoncxx::builder::stream::finalize;
}

void DatabaseServer::HandleCreateDone(const uint64_t &channel,
                                      const uint32_t &context,
                                      const uint32_t &doId) {
  auto dg = std::make_shared<Datagram>(channel, _channel,
                                       DBSERVER_CREATE_OBJECT_RESP);
  dg->AddUint32(context);
  dg->AddUint32(doId);
  PublishDatagram(dg);
}

void DatabaseServer::HandleDelete(DatagramIterator &dgi,
                                  const uint64_t &sender) {}

} // namespace Ardos
