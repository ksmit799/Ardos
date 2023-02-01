#ifndef ARDOS_STATE_SERVER_H
#define ARDOS_STATE_SERVER_H

#include <memory>

#include <amqpcpp.h>

namespace Ardos {

class StateServer {
public:
  StateServer();

private:
  AMQP::Channel *_channel;
};

} // namespace Ardos

#endif // ARDOS_STATE_SERVER_H
