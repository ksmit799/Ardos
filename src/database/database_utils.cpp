#include "database_utils.h"

#include <dcArrayParameter.h>
#include <dcAtomicField.h>
#include <dcClassParameter.h>
#include <dcField.h>
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
  // Check if we have an atomic field.
  // If we do, recursively unpack into an array.
  auto atomicField = packer.get_current_field()->as_field()->as_atomic_field();
  if (atomicField != nullptr) {
    auto arrayBuilder = builder << bsoncxx::builder::stream::open_array;

    packer.push();
    while (packer.more_nested_fields()) {
      FieldToBson(arrayBuilder, packer);
    }
    packer.pop();

    arrayBuilder << bsoncxx::builder::stream::close_array;
    return;
  }

  // Unpack the field into a bson format.
  // Note that this function can be recursively called with certain pack types.
  DCPackType packType = packer.get_pack_type();
  switch (packType) {
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
  case PT_class: {
    auto documentBuilder = builder << bsoncxx::builder::stream::open_document;

    packer.push();
    while (packer.more_nested_fields() && !packer.had_pack_error()) {
      FieldToBson(documentBuilder << packer.get_current_field()->get_name(),
                  packer);
    }
    packer.pop();

    documentBuilder << bsoncxx::builder::stream::close_document;
    break;
  }
  case PT_invalid:
  default:
    throw ConversionException("Got invalid field type");
  }

  // Throw a conversion exception if we had a packing error.
  if (packer.had_error()) {
    throw ConversionException("Packer had error");
  }
}

void DatabaseUtils::BsonToField(const DCSubatomicType &fieldType,
                                const std::string &fieldName,
                                const bsoncxx::types::bson_value::view &value,
                                const int &divisor, Datagram &dg) {
  try {
    switch (fieldType) {
    case ST_invalid:
      throw ConversionException("Got invalid field type");
    case ST_int8:
      dg.AddInt8(BsonToNumber<int8_t>(value, divisor));
      break;
    case ST_int16:
      dg.AddInt16(BsonToNumber<int16_t>(value, divisor));
      break;
    case ST_int32:
      dg.AddInt32(BsonToNumber<int32_t>(value, divisor));
      break;
    case ST_int64:
      dg.AddInt64(BsonToNumber<int64_t>(value, divisor));
      break;
    case ST_uint8:
      dg.AddUint8(BsonToNumber<uint8_t>(value, divisor));
      break;
    case ST_uint16:
      dg.AddUint16(BsonToNumber<uint16_t>(value, divisor));
      break;
    case ST_uint32:
      dg.AddUint32(BsonToNumber<uint32_t>(value, divisor));
      break;
    case ST_uint64:
      dg.AddUint64(BsonToNumber<uint64_t>(value, divisor));
      break;
    case ST_float64:
      dg.AddFloat64(BsonToNumber<double>(value, divisor));
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
    case ST_int16array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddInt16(BsonToNumber<int16_t>(it.get_value(), divisor));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_int32array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddInt32(BsonToNumber<int32_t>(it.get_value(), divisor));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_uint16array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddUint16(BsonToNumber<uint16_t>(it.get_value(), divisor));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_uint32array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddUint32(BsonToNumber<uint32_t>(it.get_value(), divisor));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_int8array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddInt8(BsonToNumber<int8_t>(it.get_value(), divisor));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_uint8array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      Datagram arrDg;
      for (const auto &it : value.get_array().value) {
        arrDg.AddUint8(BsonToNumber<uint8_t>(it.get_value(), divisor));
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
      break;
    }
    case ST_uint32uint8array: {
      if (value.type() != bsoncxx::type::k_array) {
        throw ConversionException("Expected array");
      }

      auto arr = value.get_array().value;
      auto arrLength = std::distance(arr.begin(), arr.end());

      dg.AddUint16(arrLength);
      for (size_t i = 0; i < arrLength;) {
        dg.AddUint32(BsonToNumber<uint32_t>(
            value.get_array().value[i].get_value(), divisor));
        dg.AddUint8(BsonToNumber<uint8_t>(
            value.get_array().value[i + 1].get_value(), divisor));
        i += 2;
      }
      break;
    }
    case ST_char:
      if (value.type() != bsoncxx::type::k_string &&
          value.get_string().value.length() != 1) {
        throw ConversionException("Expected char");
      }
      dg.AddUint8(value.get_string().value[0]);
      break;
    }
  } catch (ConversionException &e) {
    e.PushName(fieldName);
    throw;
  }
}

void DatabaseUtils::PackField(const DCField *field,
                              const bsoncxx::types::bson_value::view &value,
                              Datagram &dg) {
  auto atomicField = field->as_atomic_field();
  if (atomicField != nullptr) {
    // We have an atomic field.
    // E.g. setName(string);
    // Unpack each sub-element that composes the field.
    auto numFields = atomicField->get_num_elements();
    for (int i = 0; i < numFields; i++) {
      PackField(atomicField->get_element(i),
                value.get_array().value[i].get_value(), dg);
    }
    return;
  }

  auto fieldParameter = field->as_parameter();

  // Do we have a simple field (atomic) field type?
  // E.g. string, int32, uint32array, etc.
  auto fieldSimple = fieldParameter->as_simple_parameter();
  if (fieldSimple != nullptr) {
    DatabaseUtils::BsonToField(fieldSimple->get_type(), field->get_name(),
                               value, fieldSimple->get_divisor(), dg);
  }

  // Do we have a class field type?
  // E.g. MyClass, AccountInfo, etc.
  auto fieldClass = fieldParameter->as_class_parameter();
  if (fieldClass != nullptr) {
    DatabaseUtils::BsonToClass(fieldClass, value, dg);
  }

  // Do we have an array field type?
  // E.g. int32[], uint8[], MyClass[], etc.
  auto fieldArray = fieldParameter->as_array_parameter();
  if (fieldArray != nullptr) {
    // Do we have an array of a simple (atomic) type?
    auto elemParamSimple =
        fieldArray->get_element_type()->as_simple_parameter();
    if (elemParamSimple) {
      auto fieldType = elemParamSimple->get_type();

      Datagram arrDg;
      for (const auto &arrVal : value.get_array().value) {
        DatabaseUtils::BsonToField(fieldType, field->get_name(),
                                   arrVal.get_value(),
                                   elemParamSimple->get_divisor(), arrDg);
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
    }

    // Do we have an array of a class type?
    auto elemParamClass = fieldArray->get_element_type()->as_class_parameter();
    if (elemParamClass) {
      Datagram arrDg;

      for (const auto &arrVal : value.get_array().value) {
        DatabaseUtils::BsonToClass(elemParamClass, arrVal.get_value(), arrDg);
      }

      dg.AddBlob(arrDg.GetData(), arrDg.Size());
    }
  }
}

void DatabaseUtils::BsonToClass(const DCClassParameter *dclass,
                                const bsoncxx::types::bson_value::view &value,
                                Datagram &dg) {
  auto numFields = dclass->get_num_nested_fields();
  for (int i = 0; i < numFields; i++) {
    auto field = dclass->get_nested_field(i)->as_field();
    PackField(field, value.get_document().value[field->get_name()].get_value(),
              dg);
  }
}

bool DatabaseUtils::VerifyFields(const DCClass *dclass,
                                 const FieldMap &fields) {
  bool errors = false;
  for (const auto &field : fields) {
    if (!dclass->get_field_by_index(field.first->get_number())) {
      // We don't immediately break out here in case we have multiple
      // non-belonging fields.
      Logger::Error(std::format("[DB] Failed to verify field on class: {} "
                                "with non-belonging field: {}",
                                dclass->get_name(), field.first->get_name()));
      errors = true;
    }
  }

  return !errors;
}

} // namespace Ardos
