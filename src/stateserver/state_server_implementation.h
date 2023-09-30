#ifndef ARDOS_STATE_SERVER_IMPLEMENTATION_H
#define ARDOS_STATE_SERVER_IMPLEMENTATION_H

#include <cstdint>

namespace Ardos {

class StateServerImplementation {
public:
  virtual void RemoveDistributedObject(const uint32_t &doId) = 0;
};

} // namespace Ardos

#endif // ARDOS_STATE_SERVER_IMPLEMENTATION_H
