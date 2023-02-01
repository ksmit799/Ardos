#include "state_server.h"

#include "../messagedirector/message_director.h"
#include "../util/logger.h"

namespace Ardos {

StateServer::StateServer() {
  Logger::Info("Starting State Server component...");

  auto connection = MessageDirector::Instance()->GetConnection();

  _channel = new AMQP::Channel(connection);
}

} // namespace Ardos
