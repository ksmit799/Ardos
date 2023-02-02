#include "state_server.h"

#include "../messagedirector/message_director.h"
#include "../util/config.h"
#include "../util/logger.h"

namespace Ardos {

StateServer::StateServer() {
  Logger::Info("Starting State Server component...");

  // State Server configuration.
  auto config = Config::Instance()->GetNode("state-server");
  if (!config["channel"]) {
    Logger::Error("[SS] Missing or invalid channel!");
  }

  _channel = config["channel"].as<uint64_t>();

  auto globalChannel = MessageDirector::Instance()->GetGlobalChannel();
  auto localQueue = MessageDirector::Instance()->GetLocalQueue();

  // Listen to the State Server channel.
  globalChannel->bindQueue(kGlobalExchange, localQueue,
                           std::to_string(_channel));
}

} // namespace Ardos
