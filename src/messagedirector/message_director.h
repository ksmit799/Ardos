#ifndef ARDOS_MESSAGE_DIRECTOR_H
#define ARDOS_MESSAGE_DIRECTOR_H

#include <amqpcpp.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <ws28/Client.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <unordered_set>
#include <uvw.hpp>

namespace Ardos {

const std::string kGlobalExchange = "global-exchange";

class ChannelSubscriber;
class Datagram;
class MDParticipant;

class StateServer;
class ClientAgent;
class DatabaseServer;
class DatabaseStateServer;
class WebPanel;

class MessageDirector : public AMQP::ConnectionHandler {
 public:
  static MessageDirector* Instance();

  [[nodiscard]] AMQP::Channel* GetGlobalChannel() const;
  [[nodiscard]] std::string GetLocalQueue() const;

  void onData(AMQP::Connection* connection, const char* buffer,
              size_t size) override;
  void onReady(AMQP::Connection* connection) override;
  void onError(AMQP::Connection* connection, const char* message) override;
  void onClosed(AMQP::Connection* connection) override;

  void AddSubscriber(ChannelSubscriber* subscriber);
  void RemoveSubscriber(ChannelSubscriber* subscriber);

  void DeliverLocally(const std::string& routingKey,
                      const std::shared_ptr<Datagram>& dg);

  void ParticipantJoined();
  void ParticipantLeft(MDParticipant* participant);

  void HandleWeb(ws28::Client* client, nlohmann::json& data);

  [[nodiscard]] StateServer* GetStateServer() const {
    return _stateServer.get();
  }
  [[nodiscard]] ClientAgent* GetClientAgent() const {
    return _clientAgent.get();
  }
  [[nodiscard]] DatabaseServer* GetDbServer() const { return _db.get(); }
  [[nodiscard]] DatabaseStateServer* GetDbStateServer() const {
    return _dbss.get();
  }

 private:
  MessageDirector();

  void InitMetrics();

  void StartConsuming();

  void DrainLeavingSubscribers();

  static MessageDirector* _instance;

  std::unique_ptr<StateServer> _stateServer;
  std::unique_ptr<ClientAgent> _clientAgent;
  std::unique_ptr<DatabaseServer> _db;
  std::unique_ptr<DatabaseStateServer> _dbss;
  std::unique_ptr<WebPanel> _webPanel;

  std::unordered_set<ChannelSubscriber*> _subscribers;
  std::unordered_set<ChannelSubscriber*> _leavingSubscribers;
  std::unordered_set<MDParticipant*> _participants;

  // Reusable snapshot buffer for safe iteration over _subscribers. A
  // handler can synchronously construct a new ChannelSubscriber (e.g. SS
  // handling a generate spawns a DistributedObject whose ctor inserts
  // into _subscribers), which would rehash mid-iteration and invalidate
  // the active iterator. Iterating a vector copy avoids the issue;
  // making it a member reuses the heap allocation across dispatches.
  std::vector<ChannelSubscriber*> _dispatchBuffer;

  std::shared_ptr<uvw::tcp_handle> _connectHandle;
  std::shared_ptr<uvw::tcp_handle> _listenHandle;
  AMQP::Connection* _connection;
  AMQP::Channel* _globalChannel{};
  std::string _localQueue;
  std::string _consumeTag;
  std::vector<char> _frameBuffer;

  // Listen info.
  std::string _host = "127.0.0.1";
  int _port = 7100;
  // RabbitMQ connect info.
  std::string _rHost = "127.0.0.1";
  int _rPort = 5672;

  prometheus::Counter* _datagramsObservedCounter = nullptr;
  prometheus::Counter* _datagramsProcessedCounter = nullptr;
  prometheus::Histogram* _datagramsSizeHistogram = nullptr;
  prometheus::Gauge* _subscribersGauge = nullptr;
  prometheus::Gauge* _participantsGauge = nullptr;
};

}  // namespace Ardos

#endif  // ARDOS_MESSAGE_DIRECTOR_H
