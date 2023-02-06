#include "channel_subscriber.h"

#include "message_director.h"

namespace Ardos {

// We use this to keep track of which channels we have opened with RabbitMQ.
// Once a channel reaches a subscriber count of 0, we let RabbitMQ know that
// we no longer wish to be routed messages about it.
std::unordered_map<std::string, unsigned int>
    ChannelSubscriber::_globalChannels =
        std::unordered_map<std::string, unsigned int>();

ChannelSubscriber::ChannelSubscriber() {
  // Fetch the global channel and our local queue.
  _globalChannel = MessageDirector::Instance()->GetGlobalChannel();
  _localQueue = MessageDirector::Instance()->GetLocalQueue();

  // We've received a message from the MD. Handle it.
  _globalChannel->consume(_localQueue)
      .onReceived([this](const AMQP::Message &message, uint64_t deliveryTag,
                         bool redelivered) {
        // First, check if this ChannelSubscriber cares about the message.
        if (std::find(_localChannels.begin(), _localChannels.end(),
                      message.routingkey()) == _localChannels.end()) {
          return;
        }

        // We do care about the message, handle it!
        auto dg = std::make_shared<Datagram>(
            reinterpret_cast<const uint8_t *>(message.body()),
            message.bodySize());
        HandleDatagram(dg);
      });
}

void ChannelSubscriber::SubscribeChannel(const uint64_t &channel) {
  std::string channelStr = std::to_string(channel);

  // Don't add duplicate channels.
  if (std::find(_localChannels.begin(), _localChannels.end(), channelStr) !=
      _localChannels.end()) {
    return;
  }

  _localChannels.emplace_back(channelStr);

  // Next, lets check if this channel is already being listened to elsewhere.
  // If it is, increment the subscriber count.
  if (_globalChannels.contains(channelStr)) {
    _globalChannels[channelStr]++;
    return;
  }

  // Otherwise, open the channel with RabbitMQ.
  _globalChannel->bindQueue(kGlobalExchange, _localQueue, channelStr);

  // ... and register it as a newly opened global channel.
  _globalChannels[channelStr] = 1;
}

void ChannelSubscriber::UnsubscribeChannel(const uint64_t &channel) {
  std::string channelStr = std::to_string(channel);

  // Make sure we've subscribed to this channel.
  if (std::find(_localChannels.begin(), _localChannels.end(), channelStr) ==
      _localChannels.end()) {
    return;
  }

  std::erase(_localChannels, channelStr);

  // We can safely assume the channel exists in a global context.
  _globalChannels[channelStr]--;

  // If we have 0 current listeners for this channel, let RabbitMQ know we no
  // longer care about it.
  if (!_globalChannels[channelStr]) {
    _globalChannels.erase(channelStr);
    _globalChannel->unbindQueue(kGlobalExchange, _localQueue, channelStr);
  }
}

} // namespace Ardos
