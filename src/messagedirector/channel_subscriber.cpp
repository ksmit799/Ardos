#include "channel_subscriber.h"

#include "../net/datagram_iterator.h"
#include "../util/logger.h"
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

  MessageDirector::Instance()->AddSubscriber(this);
}

void ChannelSubscriber::Shutdown() {
  MessageDirector::Instance()->RemoveSubscriber(this);

  // Cleanup our local channel subscriptions.
  std::unordered_set<std::string> channels(_localChannels);
  for (const auto &channel : channels) {
    UnsubscribeChannel(std::stoull(channel));
  }

  _localChannels.clear();
}

void ChannelSubscriber::SubscribeChannel(const uint64_t &channel) {
  std::string channelStr = std::to_string(channel);

  // Don't add duplicate channels.
  if (_localChannels.contains(channelStr)) {
    return;
  }

  _localChannels.insert(channelStr);

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
  if (!_localChannels.contains(channelStr)) {
    return;
  }

  _localChannels.erase(channelStr);

  // We can safely assume the channel exists in a global context.
  _globalChannels[channelStr]--;

  // If we have 0 current listeners for this channel, let RabbitMQ know we no
  // longer care about it.
  if (!_globalChannels[channelStr]) {
    _globalChannels.erase(channelStr);
    _globalChannel->unbindQueue(kGlobalExchange, _localQueue, channelStr);
  }
}

void ChannelSubscriber::SubscribeRange(const uint32_t &min,
                                       const uint32_t &max) {}

void ChannelSubscriber::UnsubscribeRange(const uint32_t &min,
                                         const uint32_t &max) {}

void ChannelSubscriber::PublishDatagram(const std::shared_ptr<Datagram> &dg) {
  DatagramIterator dgi(dg);

  uint8_t channels = dgi.GetUint8();
  for (uint8_t i = 0; i < channels; ++i) {
    uint64_t channel = dgi.GetUint64();
    _globalChannel->publish(kGlobalExchange, std::to_string(channel),
                            reinterpret_cast<const char *>(dg->GetData()),
                            (size_t)dg->Size());
  }
}

void ChannelSubscriber::HandleUpdate(const std::string &channel,
                                     const std::shared_ptr<Datagram> &dg) {
  // First, check if this ChannelSubscriber cares about the message.
  if (!_localChannels.contains(channel)) {
    return;
  }

  // We do care about the message, handle it!
  HandleDatagram(dg);
}

} // namespace Ardos
