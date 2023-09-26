#ifndef ARDOS_DATABASE_UTILS_H
#define ARDOS_DATABASE_UTILS_H

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <dcClass.h>
#include <dcClassParameter.h>
#include <dcPacker.h>

#include "../net/datagram_iterator.h"

namespace Ardos {

typedef std::map<const DCField *, std::vector<uint8_t>> FieldMap;

/**
 * Exception thrown by many database utility functions.
 */
class ConversionException : public std::exception {
public:
  explicit ConversionException(const char *msg) : _message(msg), _what(msg) {}

  [[nodiscard]] inline const char *what() const noexcept override {
    return _what.c_str();
  }

  inline void PushName(const std::string &name) {
    _names.push_front(name);

    _what = "";
    for (const auto &it : _names) {
      if (!_what.empty()) {
        _what += ".";
      }
      _what += it;
    }

    _what += ": " + _message;
  }

private:
  std::string _message;
  std::string _what;
  std::list<std::string> _names;
};

class DatabaseUtils {
public:
  /**
   * Unpacks fields from an incoming datagram.
   * @param dgi
   * @param fieldCount
   * @param out
   * @param clearFields Whether to unpack field values or set them to
   * default/empty.
   * @return
   */
  static bool UnpackFields(DatagramIterator &dgi, const uint16_t &fieldCount,
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
  static bool UnpackFields(DatagramIterator &dgi, const uint16_t &fieldCount,
                           FieldMap &out, FieldMap &expectedOut);

  static void FieldToBson(bsoncxx::builder::stream::single_context builder,
                          DCPacker &packer);

  static void BsonToField(const DCSubatomicType &fieldType,
                          const std::string &fieldName,
                          const bsoncxx::types::bson_value::view &value,
                          Datagram &dg);

  static void PackField(const DCField *field,
                        const bsoncxx::types::bson_value::view &value,
                        Datagram &dg);

  static void BsonToClass(const DCClassParameter *dclass,
                          const bsoncxx::types::bson_value::view &value,
                          Datagram &dg);

  /**
   * Verifies the supplied fields belong to the corresponding distributed class.
   * @param dclass
   * @param fields
   * @return
   */
  static bool VerifyFields(const DCClass *dclass, const FieldMap &fields);

  /**
   * Converts a bson value to a number.
   * Specifically for handling unsigned integers.
   * @tparam T
   * @param value
   * @return
   */
  template <typename T>
  static T BsonToNumber(const bsoncxx::types::bson_value::view &value) {
    // TODO: Can we prevent a double alloc here?
    int64_t i;
    double d;

    bool isDouble = false;

    // We've got three fundamental number types
    if (value.type() == bsoncxx::type::k_int32) {
      i = value.get_int32().value;
    } else if (value.type() == bsoncxx::type::k_int64) {
      i = value.get_int64().value;
    } else if (value.type() == bsoncxx::type::k_double) {
      d = value.get_double().value;
      isDouble = true;
    } else {
      throw ConversionException("Non-numeric BSON type encountered");
    }

    if (std::numeric_limits<T>::is_integer && isDouble) {
      // The bson source value is a double, and we're expecting an integer.
      double intPart;
      if (modf(d, &intPart) != 0.0) {
        throw ConversionException("Non-integer double encountered");
      }

      if ((int64_t)d > std::numeric_limits<int64_t>::max() ||
          (int64_t)d < std::numeric_limits<int64_t>::min()) {
        throw ConversionException(
            "Excessively large (or small) double encountered");
      }

      i = static_cast<int64_t>(d);
    } else if (!std::numeric_limits<T>::is_integer && !isDouble) {
      // Integer to double. Simple enough:
      d = (double)i;
    }

    // Special case: uint64_t is stored as an int64_t. If we're doing that,
    // just cast and return:
    if (std::is_same<T, uint64_t>::value) {
      return static_cast<uint64_t>(i);
    }

    // For floating types, we'll just cast and return:
    if (!std::numeric_limits<T>::is_integer) {
      return static_cast<T>(d);
    }

    // For everything else, cast if in range:
    auto max = static_cast<int64_t>(std::numeric_limits<T>::max());
    auto min = static_cast<int64_t>(std::numeric_limits<T>::min());

    if (i > max || i < min) {
      throw ConversionException("Integer is out of range");
    }

    return static_cast<T>(i);
  }
};

} // namespace Ardos

#endif // ARDOS_DATABASE_UTILS_H
