#include "channel_subscriber.h"

#include <spdlog/spdlog.h>

#include "../net/datagram_iterator.h"
#include "message_director.h"

namespace Ardos {

// We use this to keep track of which channels we have opened with RabbitMQ.
// Once a channel reaches a subscriber count of 0, we let RabbitMQ know that
// we no longer wish to be routed messages about it.
std::unordered_map<uint64_t, unsigned int> ChannelSubscriber::_globalChannels =
    std::unordered_map<uint64_t, unsigned int>();
std::unordered_map<uint64_t, unsigned int> ChannelSubscriber::_globalBuckets =
    std::unordered_map<uint64_t, unsigned int>();

std::string ChannelSubscriber::BuildChannelRoutingKey(uint64_t channel) {
  return "chan." + std::to_string(channel >> kChannelBucketShift) + "." +
         std::to_string(channel);
}

std::string ChannelSubscriber::BuildBucketRoutingPattern(uint64_t bucket) {
  return "chan." + std::to_string(bucket) + ".*";
}

uint64_t ChannelSubscriber::ChannelFromRoutingKey(
    const std::string& routingKey) {
  auto lastDot = routingKey.rfind('.');
  if (lastDot == std::string::npos) {
    return std::stoull(routingKey);
  }
  return std::stoull(routingKey.substr(lastDot + 1));
}

ChannelSubscriber::ChannelSubscriber() {
  // Fetch the global channel and our local queue.
  _globalChannel = MessageDirector::Instance()->GetGlobalChannel();
  _localQueue = MessageDirector::Instance()->GetLocalQueue();

  MessageDirector::Instance()->AddSubscriber(this);
}

void ChannelSubscriber::Shutdown() {
  MessageDirector::Instance()->RemoveSubscriber(this);

  // Cleanup our local channel subscriptions.
  while (!_localChannels.empty()) {
    uint64_t channel = *_localChannels.begin();
    UnsubscribeChannel(channel);
  }

  // Same pattern for ranges.
  while (!_localRanges.empty()) {
    auto range = _localRanges.back();
    UnsubscribeRange(range.first, range.second);
  }
}

void ChannelSubscriber::SubscribeChannel(const uint64_t& channel) {
  // Don't add duplicate channels.
  if (!_localChannels.insert(channel).second) {
    return;
  }

  // Next, lets check if this channel is already being listened to elsewhere.
  // If it is, increment the subscriber count.
  if (_globalChannels.contains(channel)) {
    _globalChannels[channel]++;
    return;
  }

  // Otherwise, open the channel with RabbitMQ.
  _globalChannel->bindQueue(kGlobalExchange, _localQueue,
                            BuildChannelRoutingKey(channel));

  // ... and register it as a newly opened global channel.
  _globalChannels[channel] = 1;

  spdlog::get("md")->debug("Subscribe channel {} (binding new)", channel);
}

void ChannelSubscriber::UnsubscribeChannel(const uint64_t& channel) {
  // Make sure we've subscribed to this channel.
  if (!_localChannels.erase(channel)) {
    return;
  }

  // We can safely assume the channel exists in a global context.
  _globalChannels[channel]--;

  // If we have 0 current listeners for this channel, let RabbitMQ know we no
  // longer care about it.
  if (!_globalChannels[channel]) {
    _globalChannels.erase(channel);
    _globalChannel->unbindQueue(kGlobalExchange, _localQueue,
                                BuildChannelRoutingKey(channel));
  }
}

void ChannelSubscriber::SubscribeRange(const uint64_t& min,
                                       const uint64_t& max) {
  // Make sure we're not adding a duplicate range.
  auto range = std::make_pair(min, max);
  if (std::find(_localRanges.begin(), _localRanges.end(), range) !=
      _localRanges.end()) {
    return;
  }

  _localRanges.push_back(range);

  // Bind every bucket that overlaps this range. Over-delivery at the edges
  // (channels inside the end buckets but outside [min, max]) is dropped by
  // the client-side WithinLocalRange filter.
  uint64_t minBucket = min >> kChannelBucketShift;
  uint64_t maxBucket = max >> kChannelBucketShift;
  for (uint64_t bucket = minBucket; bucket <= maxBucket; ++bucket) {
    if (_globalBuckets[bucket]++ == 0) {
      _globalChannel->bindQueue(kGlobalExchange, _localQueue,
                                BuildBucketRoutingPattern(bucket));
    }
  }

  spdlog::get("md")->debug("Subscribe range [{}, {}] (buckets {}..{})", min,
                           max, minBucket, maxBucket);
}

void ChannelSubscriber::UnsubscribeRange(const uint64_t& min,
                                         const uint64_t& max) {
  auto range = std::make_pair(min, max);

  auto position = std::find(_localRanges.begin(), _localRanges.end(), range);
  if (position == _localRanges.end()) {
    return;
  }

  _localRanges.erase(position);

  // Release each bucket this range was holding. We only unbind from RabbitMQ
  // once the per-bucket ref count drops to zero, so overlapping ranges from
  // other subscribers keep their bindings alive.
  uint64_t minBucket = min >> kChannelBucketShift;
  uint64_t maxBucket = max >> kChannelBucketShift;
  for (uint64_t bucket = minBucket; bucket <= maxBucket; ++bucket) {
    if (--_globalBuckets[bucket] == 0) {
      _globalBuckets.erase(bucket);
      _globalChannel->unbindQueue(kGlobalExchange, _localQueue,
                                  BuildBucketRoutingPattern(bucket));
    }
  }
}

void ChannelSubscriber::PublishDatagram(const std::shared_ptr<Datagram>& dg) {
  DatagramIterator dgi(dg);

  // Tag every publish with our local queue name. The broker fans the message
  // out to every bound queue including our own; the consume callback drops
  // copies carrying this appID since we already delivered them in-process.
  std::string localQueue = MessageDirector::Instance()->GetLocalQueue();

  uint8_t channels = dgi.GetUint8();
  for (uint8_t i = 0; i < channels; ++i) {
    uint64_t channel = dgi.GetUint64();
    std::string routingKey = BuildChannelRoutingKey(channel);

    spdlog::get("md")->debug("Publish chan={} bucket={} size={}B", channel,
                             channel >> kChannelBucketShift, dg->Size());

    // Deliver to in-process subscribers. Avoids the subscribe-then-publish
    // race (async bindQueue not yet live) and skips the broker round-trip
    // for traffic that never needed to leave this MD. DeliverLocally no-ops
    // when nothing in this MD could match.
    MessageDirector::Instance()->DeliverLocally(routingKey, dg);

    AMQP::Envelope envelope(reinterpret_cast<const char*>(dg->GetData()),
                            (size_t)dg->Size());
    envelope.setAppID(localQueue);
    _globalChannel->publish(kGlobalExchange, routingKey, envelope);
  }
}

void ChannelSubscriber::HandleUpdate(const std::string& routingKey,
                                     const std::shared_ptr<Datagram>& dg) {
  // Routing keys look like `chan.<bucket>.<channel>`; pull the channel out
  // once for both the set lookup and the range check.
  uint64_t channel = ChannelFromRoutingKey(routingKey);

  bool inLocal = _localChannels.contains(channel);
  bool inRange = !inLocal && WithinLocalRange(channel);

  spdlog::get("md")->debug("HandleUpdate chan={} sub={} inLocal={} inRange={}",
                           channel, static_cast<const void*>(this), inLocal,
                           inRange);

  // First, check if this ChannelSubscriber cares about the message.
  if (!inLocal && !inRange) {
    return;
  }

  // We do care about the message, handle it!
  HandleDatagram(dg);
}

bool ChannelSubscriber::WithinLocalRange(uint64_t channel) {
  return std::any_of(
      _localRanges.begin(), _localRanges.end(),
      [channel](auto i) { return channel >= i.first && channel <= i.second; });
}

}  // namespace Ardos
