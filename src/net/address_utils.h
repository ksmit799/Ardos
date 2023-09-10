#ifndef ARDOS_ADDRESS_UTILS_H
#define ARDOS_ADDRESS_UTILS_H

#include <memory>
#include <string>

#include <uvw.hpp>

namespace Ardos {

class AddressUtils {
public:
  static std::string resolve_host(const std::shared_ptr<uvw::loop> &loop,
                                  const std::string &host,
                                  unsigned int port = 0);

private:
  static void *get_in_addr(struct sockaddr *sa);
};

} // namespace Ardos

#endif // ARDOS_ADDRESS_UTILS_H
