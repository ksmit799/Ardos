#ifndef ARDOS_MESSAGE_DIRECTOR_H
#define ARDOS_MESSAGE_DIRECTOR_H

#include <unordered_set>

#include <amqpcpp.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <uvw.hpp>

namespace Ardos {

const std::string kGlobalExchange = "global-exchange";

class ChannelSubscriber;

class MessageDirector : public AMQP::ConnectionHandler {
public:
  static MessageDirector *Instance();

  AMQP::Channel *GetGlobalChannel();
  std::string GetLocalQueue();

  void onData(AMQP::Connection *connection, const char *buffer,
              size_t size) override;
  void onReady(AMQP::Connection *connection) override;
  void onError(AMQP::Connection *connection, const char *message) override;
  void onClosed(AMQP::Connection *connection) override;

  void AddSubscriber(ChannelSubscriber *subscriber);
  void RemoveSubscriber(ChannelSubscriber *subscriber);

  void ParticipantJoined();
  void ParticipantLeft();

private:
  MessageDirector();

  void InitMetrics();

  void StartConsuming();

  static bool WithinGlobalRange(const std::string &channel);

  static MessageDirector *_instance;

  std::unordered_set<ChannelSubscriber *> _subscribers;
  std::unordered_set<ChannelSubscriber *> _leavingSubscribers;

  std::shared_ptr<uvw::tcp_handle> _connectHandle;
  std::shared_ptr<uvw::tcp_handle> _listenHandle;
  AMQP::Connection *_connection;
  AMQP::Channel *_globalChannel{};
  std::string _localQueue;
  std::string _consumeTag;
  std::vector<char> _frameBuffer;

  std::string _host = "127.0.0.1";
  int _port = 7100;

  prometheus::Counter *_datagramsObservedCounter = nullptr;
  prometheus::Counter *_datagramsProcessedCounter = nullptr;
  prometheus::Histogram *_datagramsSizeHistogram = nullptr;
  prometheus::Gauge *_subscribersGauge = nullptr;
  prometheus::Gauge *_participantsGauge = nullptr;
};

} // namespace Ardos

#endif // ARDOS_MESSAGE_DIRECTOR_H
