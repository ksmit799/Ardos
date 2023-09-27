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
std::map<ChannelRange, unsigned int> ChannelSubscriber::_globalRanges =
    std::map<ChannelRange, unsigned int>();

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

  // Cleanup our local range subscriptions.
  while (!_localRanges.empty()) {
    auto range = _localRanges.back();
    _localRanges.pop_back();

    UnsubscribeRange(range.first, range.second);
  }
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
                                       const uint32_t &max) {
  // Make sure we're not adding a duplicate range.
  auto range = std::make_pair(min, max);
  if (std::find(_localRanges.begin(), _localRanges.end(), range) !=
      _localRanges.end()) {
    return;
  }

  _localRanges.push_back(range);

  // Next, lets check if this channel range is already being listened to
  // elsewhere. If it is, increment the subscriber count.
  if (_globalRanges.contains(range)) {
    _globalRanges[range]++;
    return;
  }

  // Register it as a newly opened global channel range.
  _globalRanges[range] = 1;
}

void ChannelSubscriber::UnsubscribeRange(const uint32_t &min,
                                         const uint32_t &max) {
  auto range = std::make_pair(min, max);

  auto position = std::find(_localRanges.begin(), _localRanges.end(), range);
  if (position == _localRanges.end()) {
    return;
  }

  _localRanges.erase(position);

  // We can safely assume the channel range exists in a global context.
  _globalRanges[range]--;

  // If we have 0 current listeners for this channel range, let RabbitMQ know we
  // no longer care about it.
  if (!_globalRanges[range]) {
    _globalRanges.erase(range);
  }
}

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
  if (!_localChannels.contains(channel) && !WithinLocalRange(channel)) {
    return;
  }

  // We do care about the message, handle it!
  HandleDatagram(dg);
}

bool ChannelSubscriber::WithinLocalRange(const std::string &routingKey) {
  auto channel = std::stoull(routingKey);
  return std::any_of(
      _localRanges.begin(), _localRanges.end(),
      [channel](auto i) { return i.first >= channel && i.second <= channel; });
}

} // namespace Ardos
