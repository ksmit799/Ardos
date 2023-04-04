#include "database_server.h"

#include <format>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <dcClass.h>
#include <dcPacker.h>
#include <mongocxx/exception/operation_exception.hpp>

#include "../net/message_types.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "database_utils.h"

// For document, finalize, et al.
using namespace bsoncxx::builder::stream;

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

  // DoId allocation range configuration.
  auto generateParam = config["generate"];
  _minDoId = generateParam["min"].as<uint32_t>();
  _maxDoId = generateParam["max"].as<uint32_t>();

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

  // Init the Ardos "globals" document.
  // This contains information about the next available DoId to allocate and
  // any previously freed DoId's (deleted objects.)
  auto globalsExists =
      _db["globals"].find_one(document{} << "_id"
                                         << "GLOBALS" << finalize);
  if (!globalsExists) {
    // We don't have an existing globals document, insert one.
    _db["globals"].insert_one(document{} << "_id"
                                         << "GLOBALS"
                                         << "doId" << open_document << "next"
                                         << static_cast<int64_t>(_minDoId)
                                         << "free" << open_array << close_array
                                         << close_document << finalize);
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

uint32_t DatabaseServer::AllocateDoId() {
  try {
    // First, see if we have a valid next DoId for this allocation.
    auto doIdObj = _db["globals"].find_one_and_update(
        document{} << "_id"
                   << "GLOBALS"
                   << "doId.next" << open_document << "$gte"
                   << static_cast<int64_t>(_minDoId) << close_document
                   << "doId.next" << open_document << "$lte"
                   << static_cast<int64_t>(_maxDoId) << close_document
                   << finalize,
        document{} << "$inc" << open_document << "doId.next" << 1
                   << close_document << finalize);

    // We've been allocated a DoId!
    if (doIdObj) {
      return DatabaseUtils::BsonToNumber<uint32_t>(
          doIdObj->view()["doId"]["next"].get_value());
    }

    // If we couldn't find/modify a DoId within our range, check if we have any
    // freed ones we can use.
    auto freeObj = _db["globals"].find_one_and_update(
        document{} << "_id"
                   << "GLOBALS"
                   << "doId.free.0" << open_document << "$exists" << true
                   << close_document << finalize,
        document{} << "$pop" << open_document << "doId.free" << -1
                   << close_document << finalize);

    if (freeObj) {
      return DatabaseUtils::BsonToNumber<uint32_t>(
          freeObj->view()["doId"]["free"].get_array().value[0].get_value());
    }

    // We've got none left, return an invalid DoId.
    return INVALID_DO_ID;
  } catch (const ConversionException &e) {
    Logger::Error(std::format(
        "[DB] Conversion error occurred while allocating DoId: {}", e.what()));
    return INVALID_DO_ID;
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] MongoDB error occurred while allocating DoId: {}", e.what()));
    return INVALID_DO_ID;
  }
}

void DatabaseServer::FreeDoId(const uint32_t &doId) {
  Logger::Verbose(std::format("[DB] Freeing DoId: {}", doId));

  try {
    _db["globals"].update_one(
        document{} << "_id"
                   << "GLOBALS" << finalize,
        document{} << "$push" << open_document << "doId.free"
                   << static_cast<int64_t>(doId) << close_document << finalize);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(
        std::format("[DB] Failed to free DoId: {}: {}", doId, e.what()));
  }
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

  // Unpack the fields we've received in the 'create' message.
  FieldMap objectFields;
  if (!DatabaseUtils::UnpackFields(dgi, fieldCount, objectFields)) {
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

  // We can re-use the same DCPacker for each field.
  DCPacker packer;

  // Start unpacking fields into a bson stream.
  // This is essentially the same thing as handling a client/server update.
  auto builder = document{};
  try {
    for (const auto &field : objectFields) {
      packer.set_unpack_data(field.second);
      // Tell the packer we're starting to unpack the field.
      packer.begin_unpack(field.first);

      DatabaseUtils::FieldToBson(builder << field.first->get_name(), packer);

      // We've finished unpacking the field.
      packer.end_unpack();
    }
  } catch (const ConversionException &e) {
    Logger::Error(std::format(
        "[DB] Failed to unpack object fields for create: {}", e.what()));

    HandleCreateDone(sender, context, INVALID_DO_ID);
    return;
  }
  auto fields = builder << finalize;

  // Allocate a new DoId for this object.
  uint32_t doId = AllocateDoId();
  if (doId == INVALID_DO_ID) {
    HandleCreateDone(sender, context, INVALID_DO_ID);
    return;
  }

  Logger::Verbose(std::format("[DB] Inserting new {} ({}): {}",
                              dcClass->get_name(), doId,
                              bsoncxx::to_json(fields)));

  try {
    _db["objects"].insert_one(document{} << "_id" << static_cast<int64_t>(doId)
                                         << "dclass" << dcClass->get_name()
                                         << "fields" << fields << finalize);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format("[DB] Failed to insert new {} ({}): {}",
                              dcClass->get_name(), doId, e.what()));

    // Attempt to free the DoId we just allocated.
    FreeDoId(doId);

    HandleCreateDone(sender, context, INVALID_DO_ID);
    return;
  }

  // The object has been created successfully.
  HandleCreateDone(sender, context, doId);
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
                                  const uint64_t &sender) {
  uint32_t doId = dgi.GetUint32();

  try {
    auto result = _db["objects"].delete_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize);

    // Make sure we actually deleted the object.
    if (!result || result->deleted_count() != 1) {
      Logger::Error(
          std::format("[DB] Tried to delete non-existent object: {}", doId));
      return;
    }

    // Free the DoId.
    FreeDoId(doId);

    Logger::Verbose(std::format("[DB] Deleted object: {}", doId));
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while deleting object {}: {}", doId, e.what()));
  }
}

} // namespace Ardos
