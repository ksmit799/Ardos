#include "client_agent.h"

#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

ClientAgent::ClientAgent() {
  Logger::Info("Starting Client Agent component...");

  _tcpHandle = g_loop->resource<uvw::TCPHandle>();
}

} // namespace Ardos
