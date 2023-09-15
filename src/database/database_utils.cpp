#include "database_utils.h"

#include <dcField.h>
#include <dcParameter.h>
#include <dcSimpleParameter.h>

#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

bool DatabaseUtils::UnpackFields(DatagramIterator &dgi,
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

bool DatabaseUtils::UnpackFields(DatagramIterator &dgi,
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

void DatabaseUtils::FieldToBson(
    bsoncxx::builder::stream::single_context builder, DCPacker &packer) {
  // Unpack the field into a bson format.
  // Note that this function can be recursively called with certain pack types.
  DCPackType packType = packer.get_pack_type();
  switch (packType) {
  case PT_invalid:
    throw ConversionException("Got invalid field type");
  case PT_double:
    builder << bsoncxx::types::b_double{packer.unpack_double()};
    break;
  case PT_int:
    builder << bsoncxx::types::b_int32{packer.unpack_int()};
    break;
  case PT_uint:
    // bson doesn't support unsigned integers.
    // We can get around this by storing all unsigned integers as an int-64.
    // This supports up to 32-bit unsigned integers, see the 64-bit
    // implementation below.
    builder << bsoncxx::types::b_int64{packer.unpack_uint()};
    break;
  case PT_int64:
    builder << bsoncxx::types::b_int64{packer.unpack_int64()};
    break;
  case PT_uint64:
    // As above, bson doesn't support unsigned integers.
    // When storing 64-bit unsigned integers, we need to cast it to a signed
    // 64-bit and remember to cast it back when reading from the database.
    builder << bsoncxx::types::b_int64{
        static_cast<int64_t>(packer.unpack_uint64())};
    break;
  case PT_string:
    builder << bsoncxx::types::b_string{packer.unpack_string()};
    break;
  case PT_blob: {
    std::vector<unsigned char> blob = packer.unpack_blob();
    if (blob.data() == nullptr) {
      // bson gets upset if passed a nullptr here, but it's valid for
      // vector.data() to return nullptr if it's empty, so we just insert
      // nothing:
      builder << bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary, 0,
                                          (const uint8_t *)1};
    } else {
      builder << bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                                          static_cast<uint32_t>(blob.size()),
                                          blob.data()};
    }
    break;
  }
  case PT_array: {
    auto arrayBuilder = builder << bsoncxx::builder::stream::open_array;

    packer.push();
    while (packer.more_nested_fields() && !packer.had_pack_error()) {
      FieldToBson(arrayBuilder, packer);
    }
    packer.pop();

    arrayBuilder << bsoncxx::builder::stream::close_array;
    break;
  }
  case PT_field: {
    packer.push();
    while (packer.more_nested_fields() && !packer.had_pack_error()) {
      FieldToBson(builder, packer);
    }
    packer.pop();
    break;
  }
  default:
    break;
  }

  // Throw a conversion exception if we had a packing error.
  if (packer.had_error()) {
    throw ConversionException("Packer had error");
  }
}

void DatabaseUtils::BsonToField(DCField *field, const std::string &fieldName,
                                const bsoncxx::types::bson_value::view &value,
                                Datagram &dg, FieldMap &out) {
  auto fieldParameter = field->as_parameter();
  auto fieldSimple = fieldParameter->as_simple_parameter();
  auto fieldArray = fieldParameter->as_array_parameter();

  try {
    auto packType = fieldSimple->get_type();
    switch (packType) {
    case ST_invalid:
      throw ConversionException("Got invalid field type");
    case ST_int8:
      dg.AddInt8(BsonToNumber<int8_t>(value));
      break;
    case ST_int16:
      dg.AddInt16(BsonToNumber<int16_t>(value));
      break;
    case ST_int32:
      dg.AddInt32(BsonToNumber<int32_t>(value));
      break;
    case ST_int64:
      dg.AddInt64(BsonToNumber<int64_t>(value));
      break;
    case ST_uint8:
      dg.AddUint8(BsonToNumber<uint8_t>(value));
      break;
    case ST_uint16:
      dg.AddUint16(BsonToNumber<uint16_t>(value));
      break;
    case ST_uint32:
      dg.AddUint32(BsonToNumber<uint32_t>(value));
      break;
    case ST_uint64:
      dg.AddUint64(BsonToNumber<uint64_t>(value));
      break;
    case ST_float64:
      dg.AddFloat64(BsonToNumber<double>(value));
      break;
    case ST_string:
      if (value.type() != bsoncxx::type::k_string) {
        throw ConversionException("Expected string");
      }
      dg.AddString(std::string(value.get_string().value));
      break;
    case ST_blob:
    case ST_blob32:
      if (value.type() != bsoncxx::type::k_binary) {
        throw ConversionException("Expected blob");
      }
      dg.AddData(value.get_binary().bytes, value.get_binary().size);
      break;
    case ST_int16array:
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }
      dg.AddUint16(value.get_array().value.length());
      for (const auto &it : value.get_array().value) {
        dg.AddInt16(BsonToNumber<int16_t>(it.get_value()));
      }
      break;
    case ST_int32array:
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }
      dg.AddUint16(value.get_array().value.length());
      for (const auto &it : value.get_array().value) {
        dg.AddInt32(BsonToNumber<int32_t>(it.get_value()));
      }
      break;
    case ST_uint16array:
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }
      dg.AddUint16(value.get_array().value.length());
      for (const auto &it : value.get_array().value) {
        dg.AddUint16(BsonToNumber<uint16_t>(it.get_value()));
      }
      break;
    case ST_uint32array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddUint32(BsonToNumber<uint32_t>(it.get_value()));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_int8array:
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }
      dg.AddUint16(value.get_array().value.length());
      for (const auto &it : value.get_array().value) {
        dg.AddInt8(BsonToNumber<int8_t>(it.get_value()));
      }
      break;
    case ST_uint8array:
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }
      dg.AddUint16(value.get_array().value.length());
      for (const auto &it : value.get_array().value) {
        dg.AddUint8(BsonToNumber<uint8_t>(it.get_value()));
      }
      break;
    case ST_uint32uint8array:
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }
      dg.AddUint16(value.get_array().value.length());
      for (size_t i = 0; i < value.get_array().value.length();) {
        dg.AddUint32(
            BsonToNumber<uint32_t>(value.get_array().value[i].get_value()));
        dg.AddUint8(
            BsonToNumber<uint8_t>(value.get_array().value[i + 1].get_value()));
        i += 2;
      }
      break;
    case ST_char:
      if (value.type() != bsoncxx::type::k_string &&
          value.get_string().value.length() != 1) {
        throw ConversionException("Expected char");
      }
      dg.AddUint8(value.get_string().value[0]);
      break;
    }

    // Push the field data into our field map
    // and clear the datagram ready for writing.
    out[field] = dg.GetBytes();
    dg.Clear();
  } catch (ConversionException &e) {
    e.PushName(fieldName);
    throw;
  }
}

} // namespace Ardos
