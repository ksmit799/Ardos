#include "message_director.h"

#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

MessageDirector *MessageDirector::_instance = nullptr;

MessageDirector *MessageDirector::Instance() {
  if (_instance == nullptr) {
    _instance = new MessageDirector();
  }

  return _instance;
}

MessageDirector::MessageDirector() {
  Logger::Info("Starting Message Director component...");

  _tcpHandle = g_loop->resource<uvw::TCPHandle>();

  // Network configuration.
  auto config = Config::Instance()->GetNode("message-director");

  std::string host = "127.0.0.1";
  if (auto hostParam = config["rabbitmq-host"]) {
    host = hostParam.as<std::string>();
  }

  int port = 5672;
  if (auto portParam = config["rabbitmq-port"]) {
    port = portParam.as<int>();
  }

  std::string user = "guest";
  if (auto userParam = config["rabbitmq-user"]) {
    user = userParam.as<std::string>();
  }

  std::string password = "guest";
  if (auto passParam = config["rabbitmq-password"]) {
    password = passParam.as<std::string>();
  }

  // Socket events.
  _tcpHandle->once<uvw::ErrorEvent>(
      [](const uvw::ErrorEvent &event, uvw::TCPHandle &) {
        // Just die on error, the message director always needs a connection to
        // RabbitMQ.
        Logger::Error(std::format("[MD] Socket error: {}", event.what()));
        exit(1);
      });

  _tcpHandle->once<uvw::ConnectEvent>(
      [this, &user, &password](const uvw::ConnectEvent &, uvw::TCPHandle &tcp) {
        // Authenticate with the RabbitMQ cluster.
        _connection =
            new AMQP::Connection(this, AMQP::Login(user, password), "/");
        // Start reading from the socket.
        _tcpHandle->read();
      });

  _tcpHandle->on<uvw::DataEvent>(
      [this](const uvw::DataEvent &event, uvw::TCPHandle &) {
        _connection->parse(event.data.get(), event.length);
      });

  // Connect!
  _tcpHandle->connect(host, port);
}

/**
 *  Method that is called by AMQP-CPP when data has to be sent over the
 *  network. You must implement this method and send the data over a
 *  socket that is connected with RabbitMQ.
 *
 *  Note that the AMQP library does no buffering by itself. This means
 *  that this method should always send out all data or do the buffering
 *  itself.
 *
 *  @param  connection      The connection that created this output
 *  @param  buffer          Data to send
 *  @param  size            Size of the buffer
 */
void MessageDirector::onData(AMQP::Connection *connection, const char *buffer,
                             size_t size) {
  _tcpHandle->write((char *)buffer, size);
}

/**
 *  Method that is called when the login attempt succeeded. After this method
 *  is called, the connection is ready to use, and the RabbitMQ server is
 *  ready to receive instructions.
 *
 *  @param  connection      The connection that can now be used
 */
void MessageDirector::onReady(AMQP::Connection *connection) {
  Logger::Info("[MD] Connected to RabbitMQ");
}

/**
 *  When the connection ends up in an error state this method is called.
 *  This happens when data comes in that does not match the AMQP protocol,
 *  or when an error message was sent by the server to the client.
 *
 *  After this method is called, the connection no longer is in a valid
 *  state and can no longer be used.
 *
 *  @param  connection      The connection that entered the error state
 *  @param  message         Error message
 */
void MessageDirector::onError(AMQP::Connection *connection,
                              const char *message) {
  // The connection is dead at this point.
  // Log out an exception and shut everything down.
  Logger::Error(std::format("[MD] RabbitMQ error: {}", message));
  exit(1);
}

/**
 *  Method that is called when the AMQP connection was closed.
 *
 *  This is the counter part of a call to Connection::close() and it confirms
 *  that the connection was _correctly_ closed. Note that this only applies
 *  to the AMQP connection, the underlying TCP connection is not managed by
 *  AMQP-CPP and is still active.
 *
 *  @param  connection      The connection that was closed and that is now
 * unusable
 */
void MessageDirector::onClosed(AMQP::Connection *connection) {
  _tcpHandle->close();
}

} // namespace Ardos
