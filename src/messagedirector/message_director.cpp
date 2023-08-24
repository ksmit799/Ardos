#include "message_director.h"

#include "../clientagent/client_agent.h"
#ifdef ARDOS_WANT_DB_SERVER
#include "../database/database.h"
#endif
#include "../net/address_utils.h"
#include "../stateserver/state_server.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "../util/metrics.h"
#include "md_participant.h"

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

  _connectHandle = g_loop->resource<uvw::tcp_handle>();
  _listenHandle = g_loop->resource<uvw::tcp_handle>();

  auto config = Config::Instance()->GetNode("message-director");

  // Listen configuration.
  if (auto hostParam = config["host"]) {
    _host = hostParam.as<std::string>();
  }
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }

  // RabbitMQ configuration.
  std::string rHost = "127.0.0.1";
  if (auto hostParam = config["rabbitmq-host"]) {
    rHost = hostParam.as<std::string>();
  }
  int rPort = 5672;
  if (auto portParam = config["rabbitmq-port"]) {
    rPort = portParam.as<int>();
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
  _listenHandle->on<uvw::listen_event>(
      [](const uvw::listen_event &, uvw::tcp_handle &srv) {
        std::shared_ptr<uvw::tcp_handle> client =
            srv.parent().resource<uvw::tcp_handle>();
        srv.accept(*client);

        // Create a new client for this connected participant.
        // TODO: These should be tracked in a vector.
        new MDParticipant(client);
      });

  _connectHandle->on<uvw::error_event>(
      [](const uvw::error_event &event, uvw::tcp_handle &) {
        // Just die on error, the message director always needs a connection to
        // RabbitMQ.
        Logger::Error(std::format("[MD] Socket error: {}", event.what()));
        exit(1);
      });

  _connectHandle->on<uvw::connect_event>(
      [this, user, password](const uvw::connect_event &, uvw::tcp_handle &tcp) {
        // Authenticate with the RabbitMQ cluster.
        _connection =
            new AMQP::Connection(this, AMQP::Login(user, password), "/");
        // Start reading from the socket.
        _connectHandle->read();
      });

  _connectHandle->on<uvw::data_event>(
      [this](const uvw::data_event &event, uvw::tcp_handle &) {
        _connection->parse(event.data.get(), event.length);
      });

  // Initialize metrics.
  InitMetrics();

  // Start connecting/listening!
  _listenHandle->bind(_host, _port);
  _connectHandle->connect(AddressUtils::resolve_host(g_loop, rHost, rPort),
                          rPort);
}

/**
 * Returns the "global" channel used for routing messages.
 * @return
 */
AMQP::Channel *MessageDirector::GetGlobalChannel() { return _globalChannel; }

/**
 * Returns the local messaging queue for this message director.
 * @return
 */
std::string MessageDirector::GetLocalQueue() { return _localQueue; }

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
  _connectHandle->write((char *)buffer, size);
}

/**
 *  Method that is called when the login attempt succeeded. After this method
 *  is called, the connection is ready to use, and the RabbitMQ server is
 *  ready to receive instructions.
 *
 *  @param  connection      The connection that can now be used
 */
void MessageDirector::onReady(AMQP::Connection *connection) {
  // Create our "global" exchange.
  _globalChannel = new AMQP::Channel(_connection);
  _globalChannel->declareExchange(kGlobalExchange, AMQP::fanout)
      .onSuccess([this]() {
        // Create our local queue.
        // This queue is specific to this process, and will be automatically
        // deleted once it goes offline.
        _globalChannel->declareQueue(AMQP::exclusive)
            .onSuccess([this](const std::string &name, int msgCount,
                              int consumerCount) {
              _localQueue = name;

              StartConsuming();

              // TODO: We should probably have a callback for role startup to
              // happen in main.

              // Startup configured roles.
              if (Config::Instance()->GetBool("want-state-server")) {
                new StateServer();
              }

              if (Config::Instance()->GetBool("want-client-agent")) {
                new ClientAgent();
              }

              if (Config::Instance()->GetBool("want-database")) {
#ifdef ARDOS_WANT_DB_SERVER
                new Database();
#else
                Logger::Error("want-database was set to true but Ardos was "
                              "built without ARDOS_WANT_DB_SERVER");
                exit(1);
#endif
              }

              // Start listening for incoming connections.
              _listenHandle->listen();

              Logger::Verbose(std::format("[MD] Local Queue: {}", _localQueue));

              Logger::Info(
                  std::format("[MD] Listening on {}:{}", _host, _port));
            })
            .onError([](const char *message) {
              Logger::Error(std::format(
                  "[MD] Failed to declare local queue: {}", message));
              exit(1);
            });
      })
      .onError([](const char *message) {
        Logger::Error(
            std::format("[MD] Failed to declare global exchange: {}", message));
        exit(1);
      });
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
  _connectHandle->close();
  _listenHandle->close();
}

/**
 * Adds a channel subscriber to start receiving consume messages.
 * @param subscriber
 */
void MessageDirector::AddSubscriber(ChannelSubscriber *subscriber) {
  _subscribers.insert(subscriber);

  // Increment subscribers metric.
  if (_subscribersGauge) {
    _subscribersGauge->Increment();
  }
}

/**
 * Removes a channel subscriber (no longer receives consume messages.)
 * @param subscriber
 */
void MessageDirector::RemoveSubscriber(ChannelSubscriber *subscriber) {
  _subscribers.erase(subscriber);

  // Decrement subscribers metric.
  if (_subscribersGauge) {
    _subscribersGauge->Decrement();
  }
}

/**
 * Called when a participant connects.
 */
void MessageDirector::ParticipantJoined() {
  if (_participantsGauge) {
    _participantsGauge->Increment();
  }
}

/**
 * Called when a participant disconnects.
 */
void MessageDirector::ParticipantLeft() {
  if (_participantsGauge) {
    _participantsGauge->Decrement();
  }
}

/**
 * Initializes metrics collection for the message director.
 */
void MessageDirector::InitMetrics() {
  // Make sure we want to collect metrics on this cluster.
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  auto &packetsBuilder = prometheus::BuildCounter()
                             .Name("md_observed_datagrams_total")
                             .Help("Number of datagrams observed")
                             .Register(*registry);

  auto &datagramsBuilder = prometheus::BuildCounter()
                               .Name("md_handled_datagrams_total")
                               .Help("Number of datagrams handled")
                               .Register(*registry);

  auto &datagramsSizeBuilder = prometheus::BuildHistogram()
                                   .Name("md_datagrams_bytes_size")
                                   .Help("Bytes size of handled datagrams")
                                   .Register(*registry);

  auto &subscribersBuilder = prometheus::BuildGauge()
                                 .Name("md_subscribers_size")
                                 .Help("Number of registered subscribers")
                                 .Register(*registry);

  auto &participantsBuilder = prometheus::BuildGauge()
                                  .Name("md_participants_size")
                                  .Help("Number of connected participants")
                                  .Register(*registry);

  _datagramsObservedCounter = &packetsBuilder.Add({});
  _datagramsProcessedCounter = &datagramsBuilder.Add({});
  _datagramsSizeHistogram = &datagramsSizeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{1, 4, 16, 64, 256, 1024, 4096,
                                                  16384, 65536});
  _subscribersGauge = &subscribersBuilder.Add({});
  _participantsGauge = &participantsBuilder.Add({});
}

/**
 * Start consuming messages from RabbitMQ.
 * Messages are handled by each Channel Subscriber.
 */
void MessageDirector::StartConsuming() {
  _globalChannel->consume(_localQueue)
      .onSuccess([this](const std::string &tag) { _consumeTag = tag; })
      .onReceived([this](const AMQP::Message &message, uint64_t deliveryTag,
                         bool redelivered) {
        // Acknowledge the message.
        _globalChannel->ack(deliveryTag);

        // Increment observed datagrams metric.
        if (_datagramsObservedCounter) {
          _datagramsObservedCounter->Increment();
        }

        // First, check if we have at least one channel subscriber listening to
        // the channel in this cluster.
        if (!ChannelSubscriber::_globalChannels.contains(
                message.routingkey())) {
          return;
        }

        // Increment processed datagrams metric.
        if (_datagramsProcessedCounter) {
          _datagramsProcessedCounter->Increment();
        }

        // Datagram size metrics.
        if (_datagramsSizeHistogram) {
          _datagramsSizeHistogram->Observe((double)message.bodySize());
        }

        // We should only need to create one shared datagram for all
        // subscribers.
        auto dg = std::make_shared<Datagram>(
            reinterpret_cast<const uint8_t *>(message.body()),
            message.bodySize());

        // Forward the message to channel subscribers.
        // If they're not subscribed to the channel, they'll ignore it.
        for (const auto &subscriber : _subscribers) {
          subscriber->HandleUpdate(message.routingkey(), dg);
        }
      })
      .onCancelled([](const std::string &consumerTag) {
        Logger::Error("[MD] Channel consuming cancelled unexpectedly.");
      })
      .onError([](const char *message) {
        Logger::Error(std::format("[MD] Received error: {}", message));
      });
}

} // namespace Ardos
