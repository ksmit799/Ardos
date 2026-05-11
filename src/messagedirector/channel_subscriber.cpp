#include "channel_subscriber.h"

#include <spdlog/spdlog.h>

#include <algorithm>

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
std::unordered_map<uint64_t,
                   std::unordered_set<std::shared_ptr<ChannelSubscriber>>>
    ChannelSubscriber::_channelIndex;
std::unordered_map<uint64_t,
                   std::unordered_set<std::shared_ptr<ChannelSubscriber>>>
    ChannelSubscriber::_bucketIndex;

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
  // Fetch the global channel and our local queue. We can't register with the
  // MessageDirector here because shared_from_this() is not valid yet --
  // subclass factories call Init() immediately after make_shared returns.
  _globalChannel = MessageDirector::Instance()->GetGlobalChannel();
  _localQueue = MessageDirector::Instance()->GetLocalQueue();
}

void ChannelSubscriber::Init() {
  auto self = shared_from_this();
  MessageDirector::Instance()->AddSubscriber(self);

  // Backfill the routing-key index with any subscriptions that landed
  // during construction (subclass ctors may call SubscribeChannel/
  // SubscribeRange before shared_from_this is valid, in which case the
  // SubscribeChannel call skipped its index update).
  for (uint64_t channel : _localChannels) {
    _channelIndex[channel].insert(self);
  }
  for (const auto& [lo, hi] : _localRanges) {
    uint64_t minBucket = lo >> kChannelBucketShift;
    uint64_t maxBucket = hi >> kChannelBucketShift;
    for (uint64_t bucket = minBucket; bucket <= maxBucket; ++bucket) {
      _bucketIndex[bucket].insert(self);
    }
  }
}

void ChannelSubscriber::Shutdown() {
  // Hold a self-reference for the duration of Shutdown. MessageDirector's
  // _subscribers and our own _channelIndex/_bucketIndex each store
  // shared_ptrs to us; the RemoveSubscriber + UnsubscribeChannel/Range
  // calls below drop each of those refs one at a time, and on the last
  // drop the destructor would otherwise run synchronously inside the
  // erase that triggered it -- leaving Shutdown executing on freed
  // memory. Anchoring a local shared_ptr keeps us alive until the
  // method returns.
  //
  // weak_from_this().lock() returns null only when this Shutdown was
  // invoked from a destructor that's already running (refcount is
  // already 0). In that case there's nothing in the indexes to clean
  // up anyway -- the prior external Shutdown call drained them -- so
  // the loops below are no-ops and the null self is harmless.
  auto self = weak_from_this().lock();

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

  // Update the dispatch index so DeliverLocally / onReceived can find us
  // by channel without walking _subscribers. weak_from_this().lock()
  // returns null when called from a ctor (no shared_ptr exists yet);
  // Init() will backfill in that case.
  if (auto self = weak_from_this().lock()) {
    _channelIndex[channel].insert(self);
  }

  // If the channel is already bound at the broker (another subscriber in
  // this process is listening), just bump the refcount.
  if (_globalChannels.contains(channel)) {
    _globalChannels[channel]++;
    return;
  }

  // Otherwise, open the channel with RabbitMQ.
  _globalChannel->bindQueue(kGlobalExchange, _localQueue,
                            BuildChannelRoutingKey(channel));

  // ... and register it as a newly opened global channel.
  _globalChannels[channel] = 1;

  spdlog::get("md")->trace("Subscribe channel {} (binding new)", channel);
}

void ChannelSubscriber::UnsubscribeChannel(const uint64_t& channel) {
  // Make sure we've subscribed to this channel.
  if (!_localChannels.erase(channel)) {
    return;
  }

  // Remove ourselves from the dispatch index. Use find-by-pointer because
  // we may not be able to obtain a shared_ptr (Shutdown can run from the
  // destructor, where weak_from_this() is expired); the entry is keyed
  // on shared_ptr identity though, so we have to scan.
  if (auto idxIt = _channelIndex.find(channel); idxIt != _channelIndex.end()) {
    auto& set = idxIt->second;
    for (auto it = set.begin(); it != set.end(); ++it) {
      if (it->get() == this) {
        set.erase(it);
        break;
      }
    }
    if (set.empty()) {
      _channelIndex.erase(idxIt);
    }
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
  if (std::ranges::find(_localRanges, range) != _localRanges.end()) {
    return;
  }

  _localRanges.push_back(range);

  uint64_t minBucket = min >> kChannelBucketShift;
  uint64_t maxBucket = max >> kChannelBucketShift;

  // Index ourselves on every bucket the range overlaps. Same shared_ptr
  // pattern as SubscribeChannel -- Init() backfills when the call lands
  // before shared_from_this is valid.
  if (auto self = weak_from_this().lock()) {
    for (uint64_t bucket = minBucket; bucket <= maxBucket; ++bucket) {
      _bucketIndex[bucket].insert(self);
    }
  }

  // Bind every bucket that overlaps this range. Over-delivery at the edges
  // (channels inside the end buckets but outside [min, max]) is dropped by
  // the client-side WithinLocalRange filter.
  for (uint64_t bucket = minBucket; bucket <= maxBucket; ++bucket) {
    if (_globalBuckets[bucket]++ == 0) {
      _globalChannel->bindQueue(kGlobalExchange, _localQueue,
                                BuildBucketRoutingPattern(bucket));
    }
  }

  spdlog::get("md")->trace("Subscribe range [{}, {}] (buckets {}..{})", min,
                           max, minBucket, maxBucket);
}

void ChannelSubscriber::UnsubscribeRange(const uint64_t& min,
                                         const uint64_t& max) {
  auto range = std::make_pair(min, max);

  auto position = std::ranges::find(_localRanges, range);
  if (position == _localRanges.end()) {
    return;
  }

  _localRanges.erase(position);

  uint64_t minBucket = min >> kChannelBucketShift;
  uint64_t maxBucket = max >> kChannelBucketShift;

  // Drop ourselves from the bucket index for every bucket this range
  // touched. Scan-by-pointer because Shutdown may run from the destructor
  // (weak_from_this expired); the entry is keyed by shared_ptr identity
  // so we have to find ourselves the hard way. Only the matching buckets
  // are scanned -- typically a small constant.
  for (uint64_t bucket = minBucket; bucket <= maxBucket; ++bucket) {
    auto idxIt = _bucketIndex.find(bucket);
    if (idxIt == _bucketIndex.end()) {
      continue;
    }
    auto& set = idxIt->second;
    for (auto it = set.begin(); it != set.end(); ++it) {
      if (it->get() == this) {
        set.erase(it);
        break;
      }
    }
    if (set.empty()) {
      _bucketIndex.erase(idxIt);
    }
  }

  // Release each bucket this range was holding. We only unbind from RabbitMQ
  // once the per-bucket ref count drops to zero, so overlapping ranges from
  // other subscribers keep their bindings alive.
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

    spdlog::get("md")->trace("Publish chan={} bucket={} size={}B", channel,
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

bool ChannelSubscriber::WithinLocalRange(uint64_t channel) {
  return std::ranges::any_of(_localRanges, [channel](auto i) {
    return channel >= i.first && channel <= i.second;
  });
}

}  // namespace Ardos
