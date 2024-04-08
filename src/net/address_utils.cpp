#include "address_utils.h"

#include <spdlog/spdlog.h>

namespace Ardos {

std::string AddressUtils::resolve_host(const std::shared_ptr<uvw::loop> &loop,
                                       const std::string &host,
                                       unsigned int port) {
  // First, test if we have a valid IPv4 address.
  sockaddr_in sockaddr{};
  if (uv_ip4_addr(host.c_str(), port, &sockaddr) == 0) {
    return host;
  }

  // Next, test if we have a valid IPv6 address.
  sockaddr_in6 sockaddr6{};
  if (uv_ip6_addr(host.c_str(), port, &sockaddr6) == 0) {
    return host;
  }

  // We don't, lets try to resolve an IP via DNS.
  auto dnsRequest = loop->resource<uvw::get_addr_info_req>();
  auto dnsResult = dnsRequest->addr_info_sync(host, std::to_string(port));
  if (!dnsResult.first) {
    spdlog::error("[NET] Failed to resolve host address: {}:{}", host, port);
    exit(1);
  }

  char addr[INET6_ADDRSTRLEN];
  if (inet_ntop(dnsResult.second->ai_family,
                get_in_addr(dnsResult.second->ai_addr), addr,
                dnsResult.second->ai_addrlen)) {
    return {addr};
  }

  spdlog::error("[NET] Failed to parse host address: {}:{}", host, port);
  exit(1);
}

void *AddressUtils::get_in_addr(struct sockaddr *sa) {
  if (sa->sa_family == AF_INET) {
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

} // namespace Ardos
