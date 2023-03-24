#ifndef ARDOS_DATABASE_UTILS_H
#define ARDOS_DATABASE_UTILS_H

#include <bsoncxx/builder/stream/document.hpp>
#include <dcPackerInterface.h>

#include "../net/datagram_iterator.h"

namespace Ardos {

class DatabaseUtils {
public:
  static void DCToBson(bsoncxx::builder::stream::single_context builder,
                       DCPackType type, DatagramIterator &dgi);
};

} // namespace Ardos

#endif // ARDOS_DATABASE_UTILS_H
