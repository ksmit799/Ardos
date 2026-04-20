#ifndef ARDOS_CHANNEL_SUBSCRIBER_H
#define ARDOS_CHANNEL_SUBSCRIBER_H

#include <amqpcpp.h>

#include <memory>
#include <utility>

#include "../net/datagram.h"

namespace Ardos {

typedef std::pair<uint64_t, uint64_t> ChannelRange;

// Channels are bucketed by their upper bits so that range subscriptions can be
// expressed as a small number of topic bindings of the form `chan.<bucket>.*`
// rather than one binding per channel. With a 16-bit shift, each bucket covers
// 65,536 channels, so a 200M-channel range is ~3,050 bindings.
constexpr unsigned int kChannelBucketShift = 16;

class ChannelSubscriber {
 public:
  friend class MessageDirector;

  ChannelSubscriber();
  virtual ~ChannelSubscriber() = default;

  virtual void Shutdown();

  void SubscribeChannel(const uint64_t& channel);
  void UnsubscribeChannel(const uint64_t& channel);

  void SubscribeRange(const uint64_t& min, const uint64_t& max);
  void UnsubscribeRange(const uint64_t& min, const uint64_t& max);

  /**
   * Routes a datagram through the message director to the target channels.
   * @param dg
   */
  void PublishDatagram(const std::shared_ptr<Datagram>& dg);

  [[nodiscard]] std::vector<std::string> GetLocalChannels() const {
    return _localChannels;
  }

 protected:
  virtual void HandleDatagram(const std::shared_ptr<Datagram>& dg) = 0;

 private:
  void HandleUpdate(const std::string& routingKey,
                    const std::shared_ptr<Datagram>& dg);

  bool WithinLocalRange(const std::string& routingKey);

  static std::string BuildChannelRoutingKey(uint64_t channel);
  static std::string BuildBucketRoutingPattern(uint64_t bucket);
  static uint64_t ChannelFromRoutingKey(const std::string& routingKey);

  // A static map of globally registered channels.
  static std::unordered_map<std::string, unsigned int> _globalChannels;
  // Ref-counted bucket bindings. Multiple range subscriptions may overlap on
  // the same bucket; we only unbind from RabbitMQ when the count hits zero.
  static std::unordered_map<uint64_t, unsigned int> _globalBuckets;

  // List of channels that this ChannelSubscriber is listening to.
  std::vector<std::string> _localChannels;
  std::vector<ChannelRange> _localRanges;

  AMQP::Channel* _globalChannel;
  std::string _localQueue;
};

}  // namespace Ardos

#endif  // ARDOS_CHANNEL_SUBSCRIBER_H
