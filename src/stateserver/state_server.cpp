#include "state_server.h"

#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

StateServer::StateServer() {
  Logger::Info("Starting State Server component...");

  _tcpHandle = g_loop->resource<uvw::TCPHandle>();

  // Network configuration.
  auto config = Config::Instance()->GetNode("state-server");

  std::string host = "127.0.0.1";
  if (auto hostParam = config["host"]) {
    host = hostParam.as<std::string>();
  }

  int port = 4242;
  if (auto portParam = config["port"]) {
    port = portParam.as<int>();
  }

  // Socket events.
  _tcpHandle->on<uvw::ListenEvent>([this](const uvw::ListenEvent &, uvw::TCPHandle &srv) {
    std::shared_ptr<uvw::TCPHandle> client = srv.loop().resource<uvw::TCPHandle>();
    srv.accept(*client);
  });

  // Start listening!
  _tcpHandle->bind(host, port);
  _tcpHandle->listen();

  Logger::Info(std::format("State Server listening on {}:{}", host, port));
}

} // namespace Ardos
