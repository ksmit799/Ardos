#ifndef ARDOS_CHANNEL_SUBSCRIBER_H
#define ARDOS_CHANNEL_SUBSCRIBER_H

#include <memory>
#include <utility>

#include <amqpcpp.h>

#include "../net/datagram.h"

namespace Ardos {

typedef std::pair<unsigned int, unsigned int> ChannelRange;

class ChannelSubscriber {
public:
  friend class MessageDirector;

  ChannelSubscriber();
  virtual ~ChannelSubscriber() = default;

  virtual void Shutdown();

  void SubscribeChannel(const uint64_t &channel);
  void UnsubscribeChannel(const uint64_t &channel);

  void SubscribeRange(const uint32_t &min, const uint32_t &max);
  void UnsubscribeRange(const uint32_t &min, const uint32_t &max);

  /**
   * Routes a datagram through the message director to the target channels.
   * @param dg
   */
  void PublishDatagram(const std::shared_ptr<Datagram> &dg);

protected:
  virtual void HandleDatagram(const std::shared_ptr<Datagram> &dg) = 0;

private:
  void HandleUpdate(const std::string &channel,
                    const std::shared_ptr<Datagram> &dg);

  // A static map of globally registered channels.
  static std::unordered_map<std::string, unsigned int> _globalChannels;

  // List of channels that this ChannelSubscriber is listening to.
  std::unordered_set<std::string> _localChannels;
  std::vector<ChannelRange> _localRanges;

  AMQP::Channel *_globalChannel;
  std::string _localQueue;
};

} // namespace Ardos

#endif // ARDOS_CHANNEL_SUBSCRIBER_H
