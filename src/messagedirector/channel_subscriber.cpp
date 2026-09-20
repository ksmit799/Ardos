#include "channel_subscriber.h"

#include <spdlog/spdlog.h>

#include <algorithm>

#include "message_director.h"

namespace Ardos {

std::unordered_map<uint64_t, unsigned int> ChannelSubscriber::_globalChannels;
std::map<ChannelRange, unsigned int> ChannelSubscriber::_globalRanges;
std::unordered_map<uint64_t,
                   std::unordered_set<std::shared_ptr<ChannelSubscriber>>>
    ChannelSubscriber::_channelIndex;
std::vector<ChannelSubscriber::LocalRange> ChannelSubscriber::_rangeIndex;

ChannelSubscriber::ChannelSubscriber() = default;

void ChannelSubscriber::Init() {
  auto self = shared_from_this();
  MessageDirector::Instance()->AddSubscriber(self);

  // Backfill the dispatch indexes with any subscriptions that landed
  // during construction, subclass ctors may subscribe before
  // shared_from_this is valid, those calls skipped their index update.
  for (uint64_t channel : _localChannels) {
    _channelIndex[channel].insert(self);
  }
  for (const auto& range : _localRanges) {
    bool indexed = false;
    for (const auto& entry : _rangeIndex) {
      if (entry.sub.get() == this && entry.lo == range.first &&
          entry.hi == range.second) {
        indexed = true;
        break;
      }
    }
    if (!indexed) {
      _rangeIndex.push_back(
          {.lo = range.first, .hi = range.second, .sub = self});
    }
  }
}

void ChannelSubscriber::Shutdown() {
  // Anchor self for the duration of the method. RemoveSubscriber and
  // each UnsubscribeChannel/Range below drops a shared_ptr ref; without
  // this pin the last drop would destroy `this` inside the erase. Null
  // when called from a destructor (indexes already drained, loops no-op).
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

  // Update the dispatch index. weak_from_this().lock() returns null when
  // called from a ctor (no shared_ptr exists yet); Init() will backfill.
  if (auto self = weak_from_this().lock()) {
    _channelIndex[channel].insert(self);
  }

  // First local subscriber on this channel, tell the mesh.
  if (++_globalChannels[channel] == 1) {
    MessageDirector::Instance()->BroadcastAddChannel(channel);
    spdlog::get("md")->trace("Subscribe channel {} (advertising)", channel);
  }
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

  // Last local subscriber gone, withdraw it from the mesh.
  if (--_globalChannels[channel] == 0) {
    _globalChannels.erase(channel);
    MessageDirector::Instance()->BroadcastRemoveChannel(channel);
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

  // Same shared_ptr pattern as SubscribeChannel, Init() backfills when
  // the call lands before shared_from_this is valid.
  if (auto self = weak_from_this().lock()) {
    _rangeIndex.push_back({.lo = min, .hi = max, .sub = self});
  }

  // First local subscriber on this exact range, tell the mesh.
  // Overlapping ranges advertise separately, peers dedupe on delivery.
  if (++_globalRanges[range] == 1) {
    MessageDirector::Instance()->BroadcastAddRange(min, max);
    spdlog::get("md")->trace("Subscribe range [{}, {}] (advertising)", min,
                             max);
  }
}

void ChannelSubscriber::UnsubscribeRange(const uint64_t& min,
                                         const uint64_t& max) {
  auto range = std::make_pair(min, max);

  auto position = std::ranges::find(_localRanges, range);
  if (position == _localRanges.end()) {
    return;
  }

  _localRanges.erase(position);

  // Scan-by-pointer for the same destructor reason as UnsubscribeChannel.
  auto idxIt = std::ranges::find_if(_rangeIndex, [&](const auto& entry) {
    return entry.sub.get() == this && entry.lo == min && entry.hi == max;
  });
  if (idxIt != _rangeIndex.end()) {
    _rangeIndex.erase(idxIt);
  }

  // Last local subscriber gone, withdraw it from the mesh.
  if (--_globalRanges[range] == 0) {
    _globalRanges.erase(range);
    MessageDirector::Instance()->BroadcastRemoveRange(min, max);
  }
}

void ChannelSubscriber::PublishDatagram(const std::shared_ptr<Datagram>& dg) {
  MessageDirector::Instance()->RouteDatagram(dg);
}

}  // namespace Ardos
