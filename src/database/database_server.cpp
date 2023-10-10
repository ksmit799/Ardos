#include "database_server.h"

#include <format>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <dcClass.h>
#include <dcPacker.h>
#include <mongocxx/exception/operation_exception.hpp>

#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "../util/metrics.h"
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

  // Initialize metrics.
  InitMetrics();

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
      HandleDelete(dgi);
      break;
    case DBSERVER_OBJECT_GET_ALL:
      HandleGetAll(dgi, sender);
      break;
    case DBSERVER_OBJECT_GET_FIELD:
    case DBSERVER_OBJECT_GET_FIELDS:
      HandleGetField(dgi, sender, msgType == DBSERVER_OBJECT_GET_FIELDS);
      break;
    case DBSERVER_OBJECT_SET_FIELD:
    case DBSERVER_OBJECT_SET_FIELDS:
      HandleSetField(dgi, msgType == DBSERVER_OBJECT_SET_FIELDS);
      break;
    case DBSERVER_OBJECT_DELETE_FIELD:
    case DBSERVER_OBJECT_DELETE_FIELDS:
      // TODO: Implement this.
      Logger::Error("[DB] OBJECT_DELETE_FIELD(S) NOT YET IMPLEMENTED!");
      break;
    case DBSERVER_OBJECT_SET_FIELD_IF_EMPTY:
      // TODO: Implement this.
      Logger::Error("[DB] OBJECT_SET_FIELD_IF_EMPTY NOT YET IMPLEMENTED!");
      break;
    case DBSERVER_OBJECT_SET_FIELD_IF_EQUALS:
    case DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS:
      HandleSetFieldEquals(dgi, sender,
                           msgType == DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS);
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
      if (_freeChannelsGauge) {
        _freeChannelsGauge->Decrement();
      }

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
      if (_freeChannelsGauge) {
        _freeChannelsGauge->Decrement();
      }

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

    if (_freeChannelsGauge) {
      _freeChannelsGauge->Increment();
    }
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
  if (!DatabaseUtils::VerifyFields(dcClass, objectFields)) {
    Logger::Error(std::format(
        "[DB] Failed to create object: {} with non-belonging fields",
        dcClass->get_name()));
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

  // Start unpacking fields into a bson document.
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

void DatabaseServer::HandleDelete(DatagramIterator &dgi) {
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

void DatabaseServer::HandleGetAll(DatagramIterator &dgi,
                                  const uint64_t &sender) {
  uint32_t context = dgi.GetUint32();
  uint32_t doId = dgi.GetUint32();

  std::optional<bsoncxx::document::value> obj;
  try {
    obj = _db["objects"].find_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while fetching object {}: {}", doId, e.what()));
    HandleContextFailure(DBSERVER_OBJECT_GET_ALL_RESP, sender, context);
    return;
  }

  if (!obj) {
    Logger::Error(
        std::format("[DB] Failed to fetch non-existent object: {}", doId));
    HandleContextFailure(DBSERVER_OBJECT_GET_ALL_RESP, sender, context);
    return;
  }

  auto dclassName = std::string(obj->view()["dclass"].get_string().value);

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class_by_name(dclassName);
  if (!dcClass) {
    Logger::Error(std::format(
        "[DB] Encountered unknown dclass while fetching object {}: {}", doId,
        dclassName));
    HandleContextFailure(DBSERVER_OBJECT_GET_ALL_RESP, sender, context);
    return;
  }

  // Unpack fields.
  auto fields = obj->view()["fields"].get_document().value;

  FieldMap objectFields;
  Datagram objectDg;
  try {
    for (const auto &it : fields) {
      auto fieldName = std::string(it.key());

      DCField *field = dcClass->get_field_by_name(fieldName);
      if (!field) {
        Logger::Warn(std::format("[DB] Encountered unexpected field while "
                                 "fetching object {}: {} - {}",
                                 doId, dclassName, fieldName));
        continue;
      }

      // Pack the field into our object datagram.
      DatabaseUtils::PackField(field, it.get_value(), objectDg);

      // Push the field data into our field map
      // and clear the datagram ready for writing.
      objectFields[field] = objectDg.GetBytes();
      objectDg.Clear();
    }
  } catch (const ConversionException &e) {
    Logger::Error(
        std::format("[DB] Failed to unpack field fetching object {}: {} - {}",
                    doId, dclassName, e.what()));
    HandleContextFailure(DBSERVER_OBJECT_GET_ALL_RESP, sender, context);
    return;
  }

  auto dg = std::make_shared<Datagram>(sender, _channel,
                                       DBSERVER_OBJECT_GET_ALL_RESP);
  dg->AddUint32(context);
  dg->AddBool(true);
  dg->AddUint16(dcClass->get_number());
  dg->AddUint16(objectFields.size()); // Field count.
  for (const auto &it : objectFields) {
    dg->AddUint16(it.first->get_number());
    dg->AddData(it.second);
  }
  PublishDatagram(dg);
}

void DatabaseServer::HandleGetField(DatagramIterator &dgi,
                                    const uint64_t &sender,
                                    const bool &multiple) {
  auto ctx = dgi.GetUint32();
  auto doId = dgi.GetUint32();
  auto fieldCount = multiple ? dgi.GetUint16() : 1;

  auto responseType = multiple ? DBSERVER_OBJECT_GET_FIELDS_RESP
                               : DBSERVER_OBJECT_GET_FIELD_RESP;

  std::optional<bsoncxx::document::value> obj;
  try {
    obj = _db["objects"].find_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while setting field(s) on object {}: {}", doId,
        e.what()));
    return;
  }

  if (!obj) {
    Logger::Error(std::format(
        "[DB] Failed to set field(s) on non-existent object: {}", doId));
    return;
  }

  auto dclassName = std::string(obj->view()["dclass"].get_string().value);

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class_by_name(dclassName);
  if (!dcClass) {
    Logger::Error(std::format(
        "[DB] Received set field(s) for unknown distributed class {}: {}", doId,
        dclassName));
    return;
  }

  // Unpack fields.
  auto fields = obj->view()["fields"].get_document().value;

  FieldMap objectFields;
  Datagram objectDg;
  try {
    for (size_t i = 0; i < fieldCount; i++) {
      // Fetch the field by number.
      auto fieldNum = dgi.GetUint16();
      DCField *field = dcClass->get_field_by_index(fieldNum);
      if (!field) {
        Logger::Error(std::format("[DB] Encountered unexpected field while "
                                  "fetching object {}: {} - {}",
                                  doId, dclassName, fieldNum));
        HandleContextFailure(responseType, sender, ctx);
        return;
      }

      // Pack the field into our object datagram.
      DatabaseUtils::PackField(field, fields[field->get_name()].get_value(),
                               objectDg);

      // Push the field data into our field map
      // and clear the datagram ready for writing.
      objectFields[field] = objectDg.GetBytes();
      objectDg.Clear();
    }
  } catch (const ConversionException &e) {
    Logger::Error(
        std::format("[DB] Failed to unpack field fetching object {}: {} - {}",
                    doId, dclassName, e.what()));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  auto dg = std::make_shared<Datagram>(sender, _channel, responseType);
  dg->AddUint32(ctx);
  dg->AddBool(true);
  if (multiple) {
    dg->AddUint16(objectFields.size());
  }
  for (const auto &it : objectFields) {
    dg->AddUint16(it.first->get_number());
    dg->AddData(it.second);
  }
  PublishDatagram(dg);
}

void DatabaseServer::HandleSetField(DatagramIterator &dgi,
                                    const bool &multiple) {
  auto doId = dgi.GetUint32();
  auto fieldCount = multiple ? dgi.GetUint16() : 1;

  std::optional<bsoncxx::document::value> obj;
  try {
    obj = _db["objects"].find_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while setting field(s) on object {}: {}", doId,
        e.what()));
    return;
  }

  if (!obj) {
    Logger::Error(std::format(
        "[DB] Failed to set field(s) on non-existent object: {}", doId));
    return;
  }

  auto dclassName = std::string(obj->view()["dclass"].get_string().value);

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class_by_name(dclassName);
  if (!dcClass) {
    Logger::Error(std::format(
        "[DB] Received set field(s) for unknown distributed class {}: {}", doId,
        dclassName));
    return;
  }

  // Unpack the fields we've received in the 'set' message.
  FieldMap objectFields;
  if (!DatabaseUtils::UnpackFields(dgi, fieldCount, objectFields)) {
    Logger::Error(
        std::format("[DB] Failed to unpack set field(s) for object: {}", doId));
    return;
  }

  // Make sure that all present fields actually belong to the distributed class.
  if (!DatabaseUtils::VerifyFields(dcClass, objectFields)) {
    Logger::Error(std::format("[DB] Failed to verify fields on object {}: {} ",
                              doId, dclassName));
    return;
  }

  // We can re-use the same DCPacker for each field.
  DCPacker packer;

  // Start unpacking fields into a bson document.
  auto builder = document{};
  try {
    for (const auto &field : objectFields) {
      packer.set_unpack_data(field.second);
      // Tell the packer we're starting to unpack the field.
      packer.begin_unpack(field.first);

      DatabaseUtils::FieldToBson(
          builder << std::string_view("fields." + field.first->get_name()),
          packer);

      // We've finished unpacking the field.
      packer.end_unpack();
    }
  } catch (const ConversionException &e) {
    Logger::Error(std::format(
        "[DB] Failed to unpack object fields for set field(s) {}: {}", doId,
        e.what()));
    return;
  }

  auto fieldBuilder = builder << finalize;
  auto fieldUpdate = document{} << "$set" << fieldBuilder << finalize;

  try {
    auto updateOperation = _db["objects"].update_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize,
        fieldUpdate.view());

    if (!updateOperation) {
      Logger::Error(std::format(
          "[DB] Set field(s) update operation failed for object {}", doId));
      return;
    }

    Logger::Verbose(std::format("[DB] Set field(s) for object {}: {}", doId,
                                bsoncxx::to_json(fieldBuilder.view())));
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while setting field(s) on object {}: {}", doId,
        e.what()));
  }
}

void DatabaseServer::HandleSetFieldEquals(DatagramIterator &dgi,
                                          const uint64_t &sender,
                                          const bool &multiple) {
  auto ctx = dgi.GetUint32();
  auto doId = dgi.GetUint32();
  auto fieldCount = multiple ? dgi.GetUint16() : 1;

  auto responseType = multiple ? DBSERVER_OBJECT_SET_FIELDS_IF_EQUALS_RESP
                               : DBSERVER_OBJECT_SET_FIELD_IF_EQUALS_RESP;

  std::optional<bsoncxx::document::value> obj;
  try {
    obj = _db["objects"].find_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while setting field(s) equals on object {}: {}",
        doId, e.what()));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  if (!obj) {
    Logger::Error(std::format(
        "[DB] Failed to set field(s) equals on non-existent object: {}", doId));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  auto dclassName = std::string(obj->view()["dclass"].get_string().value);

  // Make sure we have a valid distributed class.
  DCClass *dcClass = g_dc_file->get_class_by_name(dclassName);
  if (!dcClass) {
    Logger::Error(std::format("[DB] Received set field(s) equals for unknown "
                              "distributed class {}: {}",
                              doId, dclassName));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  // Unpack the fields we've received in the 'set' message.
  FieldMap objectFields;
  FieldMap expectedFields;
  if (!DatabaseUtils::UnpackFields(dgi, fieldCount, objectFields,
                                   expectedFields)) {
    Logger::Error(std::format(
        "[DB] Failed to unpack set field(s) equals for object: {}", doId));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  // Make sure that all present fields actually belong to the distributed class.
  if (!DatabaseUtils::VerifyFields(dcClass, objectFields)) {
    Logger::Error(std::format(
        "[DB] Failed to verify set field(s) equals for object {}: {} ", doId,
        dclassName));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }
  if (!DatabaseUtils::VerifyFields(dcClass, expectedFields)) {
    Logger::Error(std::format(
        "[DB] Failed to verify expected field(s) equals for object {}: {} ",
        doId, dclassName));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  auto fields = obj->view()["fields"].get_document().value;

  // First, make sure our expected fields match.
  FieldMap failedFields;
  Datagram objectDg;
  for (const auto &it : expectedFields) {
    auto fieldValue = fields[it.first->get_name()];
    if (!fieldValue) {
      // Hmm, the field doesn't exist at all.
      // Just insert an empty vector as its data.
      failedFields[it.first] = std::vector<uint8_t>();
      Logger::Verbose(std::format("[DB] Missing expected field {} in set "
                                  "field(s) equals for object {}: {}",
                                  it.first->get_name(), doId, dclassName));
      continue;
    }

    // Pack the field from the database.
    // This gets it into the same format as the expected field.
    DatabaseUtils::PackField(it.first, fieldValue.get_value(), objectDg);

    if (it.second != objectDg.GetBytes()) {
      // The field exists but the actual/expected data is mismatched.
      failedFields[it.first] = objectDg.GetBytes();
      Logger::Verbose(std::format("[DB] Mismatched expected field {} in set "
                                  "field(s) equals for object {}: {}",
                                  it.first->get_name(), doId, dclassName));
      continue;
    }

    // Clear the object dg ready for iterating again.
    objectDg.Clear();
  }

  // One or more fields failed to validate, notify the sender.
  if (!failedFields.empty()) {
    auto dg = std::make_shared<Datagram>(sender, _channel, responseType);
    dg->AddUint32(ctx);
    dg->AddBool(false);
    if (multiple) {
      dg->AddUint16(failedFields.size());
    }
    for (const auto &it : failedFields) {
      dg->AddUint16(it.first->get_number());
      dg->AddData(it.second);
    }
    PublishDatagram(dg);
    return;
  }

  // We can re-use the same DCPacker for each field.
  DCPacker packer;

  // Start unpacking fields into a bson document.
  auto builder = document{};
  try {
    for (const auto &field : objectFields) {
      packer.set_unpack_data(field.second);
      // Tell the packer we're starting to unpack the field.
      packer.begin_unpack(field.first);

      DatabaseUtils::FieldToBson(
          builder << std::string_view("fields." + field.first->get_name()),
          packer);

      // We've finished unpacking the field.
      packer.end_unpack();
    }
  } catch (const ConversionException &e) {
    Logger::Error(std::format(
        "[DB] Failed to unpack object fields for set field(s) equals {}: {}",
        doId, e.what()));
    HandleContextFailure(responseType, sender, ctx);
    return;
  }

  auto fieldBuilder = builder << finalize;
  auto fieldUpdate = document{} << "$set" << fieldBuilder << finalize;

  try {
    auto updateOperation = _db["objects"].update_one(
        document{} << "_id" << static_cast<int64_t>(doId) << finalize,
        fieldUpdate.view());

    if (!updateOperation) {
      Logger::Error(std::format(
          "[DB] Set field(s) equals operation failed for object {}", doId));
      HandleContextFailure(responseType, sender, ctx);
      return;
    }

    Logger::Verbose(std::format("[DB] Set field(s) equals for object {}: {}",
                                doId, bsoncxx::to_json(fieldBuilder.view())));
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format(
        "[DB] Unexpected error while setting field(s) equals on object {}: {}",
        doId, e.what()));
    HandleContextFailure(responseType, sender, ctx);
  }

  // Success! Notify the sender.
  auto dg = std::make_shared<Datagram>(sender, _channel, responseType);
  dg->AddUint32(ctx);
  dg->AddBool(true);
  PublishDatagram(dg);
}

void DatabaseServer::HandleContextFailure(const MessageTypes &type,
                                          const uint64_t &channel,
                                          const uint32_t &context) {
  auto dg = std::make_shared<Datagram>(channel, _channel, type);
  dg->AddUint32(context);
  dg->AddBool(false);
  PublishDatagram(dg);
}

void DatabaseServer::InitMetrics() {
  // Make sure we want to collect metrics on this cluster.
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  auto &freeChannelsBuilder = prometheus::BuildGauge()
                                  .Name("db_free_channels_size")
                                  .Help("Number of free channels")
                                  .Register(*registry);

  auto &opsCompletedBuilder =
      prometheus::BuildCounter()
          .Name("db_ops_completed")
          .Help("Number of successful database operations")
          .Register(*registry);

  auto &opsFailedBuilder = prometheus::BuildCounter()
                               .Name("db_ops_failed")
                               .Help("Number of failed database operations")
                               .Register(*registry);

  auto &opsTimeBuilder =
      prometheus::BuildHistogram()
          .Name("db_ops_time")
          .Help("Time taken for a successful database operation to complete")
          .Register(*registry);

  _freeChannelsGauge = &freeChannelsBuilder.Add({});

  // Map operation types to a human-readable string.
  // These will be displayed in Prometheus/Grafana.
  const std::vector<std::pair<OperationType, std::string>> OPERATIONS = {
      {OperationType::CREATE_OBJECT, "create_object"},
      {OperationType::DELETE_OBJECT, "delete_object"},
      {OperationType::GET_OBJECT, "get_object"},
      {OperationType::GET_OBJECT_FIELDS, "get_fields"},
      {OperationType::SET_OBJECT_FIELDS, "set_fields"},
      {OperationType::UPDATE_OBJECT_FIELDS, "update_fields"}};

  // Populate operation maps.
  for (const auto &opType : OPERATIONS) {
    _opsCompleted[opType.first] =
        &opsCompletedBuilder.Add({{"op_type", opType.second}});
    _opsFailed[opType.first] =
        &opsFailedBuilder.Add({{"op_type", opType.second}});
    _opsCompletionTime[opType.first] = &opsTimeBuilder.Add(
        {{"op_type", opType.second}},
        prometheus::Histogram::BucketBoundaries{0, 500, 1000, 1500, 2000, 2500,
                                                3000, 3500, 4000, 4500, 5000});
  }

  // Calculate the number of free channels we have left to allocate.
  InitFreeChannelsMetric();
}

void DatabaseServer::InitFreeChannelsMetric() {
  try {
    // Get the next DoId we have ready to allocate.
    auto doIdObj = _db["globals"].find_one(
        document{} << "_id"
                   << "GLOBALS"
                   << "doId.next" << open_document << "$gte"
                   << static_cast<int64_t>(_minDoId) << close_document
                   << "doId.next" << open_document << "$lte"
                   << static_cast<int64_t>(_maxDoId) << close_document
                   << finalize);

    if (!doIdObj) {
      _freeChannelsGauge->Set(0);
      return;
    }

    auto currDoId = DatabaseUtils::BsonToNumber<uint32_t>(
        doIdObj->view()["doId"]["next"].get_value());

    auto freeDoIdArr = doIdObj->view()["doId"]["free"].get_array().value;
    auto freeDoIds = std::distance(freeDoIdArr.begin(), freeDoIdArr.end());

    _freeChannelsGauge->Set((double)(_maxDoId - currDoId + freeDoIds));
  } catch (const ConversionException &e) {
    Logger::Error(std::format("[DB] Conversion error occurred while "
                              "calculating free channel metrics: {}",
                              e.what()));
    _freeChannelsGauge->Set(0);
  } catch (const mongocxx::operation_exception &e) {
    Logger::Error(std::format("[DB] MongoDB error occurred while calculating "
                              "free channel metrics: {}",
                              e.what()));
    _freeChannelsGauge->Set(0);
  }
}

} // namespace Ardos
