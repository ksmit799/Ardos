#include "database_utils.h"

namespace Ardos {

void DatabaseUtils::DCToBson(bsoncxx::builder::stream::single_context builder,
                             DCPackType type, DatagramIterator &dgi) {
  switch (type) {
  case PT_invalid:
    break;
  case PT_double:
    break;
  case PT_int:
  case PT_uint:
    builder << bsoncxx::types::b_int32{dgi.GetUint8()};
    break;
  case PT_int64:
  case PT_uint64:
    builder << bsoncxx::types::b_int64{dgi.GetInt64()};
    break;
  case PT_string:
    builder << bsoncxx::types::b_string{dgi.GetString()};
    break;
  case PT_blob:
    builder << bsoncxx::types::b_int32{dgi.GetUint8()};
    break;
  case PT_array:
    break;
  case PT_field:
    break;
  case PT_class:
    break;
  case PT_switch:
    break;
  }
}

} // namespace Ardos
