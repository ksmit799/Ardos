#ifndef ARDOS_CHANNEL_SUBSCRIBER_H
#define ARDOS_CHANNEL_SUBSCRIBER_H

#include <memory>

#include <amqpcpp.h>

#include "../net/datagram.h"

namespace Ardos {

class ChannelSubscriber {
public:
  ChannelSubscriber();

  void SubscribeChannel(const uint64_t &channel);
  void UnsubscribeChannel(const uint64_t &channel);

protected:
  virtual void HandleDatagram(const std::shared_ptr<Datagram> &dg) = 0;

private:
  // A static map of globally registered channels.
  static std::unordered_map<std::string, unsigned int> _globalChannels;

  // List of channels that this ChannelSubscriber is listening to.
  std::vector<std::string> _localChannels;

  AMQP::Channel *_globalChannel;
  std::string _localQueue;
};

} // namespace Ardos

#endif // ARDOS_CHANNEL_SUBSCRIBER_H
