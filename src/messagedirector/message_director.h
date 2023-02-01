#ifndef ARDOS_MESSAGE_DIRECTOR_H
#define ARDOS_MESSAGE_DIRECTOR_H

#include <amqpcpp.h>
#include <uvw.hpp>

namespace Ardos {

class MessageDirector : public AMQP::ConnectionHandler {
public:
  static MessageDirector *Instance();

  AMQP::Connection *GetConnection();

  void onData(AMQP::Connection *connection, const char *buffer,
              size_t size) override;
  void onReady(AMQP::Connection *connection) override;
  void onError(AMQP::Connection *connection, const char *message) override;
  void onClosed(AMQP::Connection *connection) override;

private:
  MessageDirector();

  static MessageDirector *_instance;

  std::shared_ptr<uvw::TCPHandle> _connectHandle;
  std::shared_ptr<uvw::TCPHandle> _listenHandle;
  AMQP::Connection *_connection;

  std::string _host = "127.0.0.1";
  int _port = 7100;
};

} // namespace Ardos

#endif // ARDOS_MESSAGE_DIRECTOR_H