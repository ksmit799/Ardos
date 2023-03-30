#ifndef ARDOS_DATABASE_UTILS_H
#define ARDOS_DATABASE_UTILS_H

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/types/bson_value/view.hpp>
#include <dcPacker.h>

#include "../net/datagram_iterator.h"

namespace Ardos {

typedef std::map<const DCField *, std::vector<uint8_t>> FieldMap;

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

  template <typename T>
  static T BsonToNumber(const bsoncxx::types::bson_value::view &value);
};

} // namespace Ardos

#endif // ARDOS_DATABASE_UTILS_H
