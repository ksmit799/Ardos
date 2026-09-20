#ifndef ARDOS_CHANNEL_SUBSCRIBER_H
#define ARDOS_CHANNEL_SUBSCRIBER_H

#include <map>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../net/datagram.h"

namespace Ardos {

using ChannelRange = std::pair<uint64_t, uint64_t>;

class ChannelSubscriber
    : public std::enable_shared_from_this<ChannelSubscriber> {
 public:
  friend class MessageDirector;

  ChannelSubscriber();
  virtual ~ChannelSubscriber() = default;

  // Register this subscriber with the MessageDirector. Must be called once,
  // after construction, because shared_from_this() is not valid inside the
  // constructor. Subclasses use a static Create<T>(...) factory that does
  // make_shared<T>() and then calls Init().
  virtual void Init();

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

  [[nodiscard]] const std::unordered_set<uint64_t>& GetLocalChannels() const {
    return _localChannels;
  }

  // What this whole instance subscribes to, the mesh advertises these
  // and replays them as the join snapshot.
  static const std::unordered_map<uint64_t, unsigned int>&
  GetAdvertisedChannels() {
    return _globalChannels;
  }
  static const std::map<ChannelRange, unsigned int>& GetAdvertisedRanges() {
    return _globalRanges;
  }

 protected:
  virtual void HandleDatagram(const std::shared_ptr<Datagram>& dg) = 0;

 private:
  // Instance wide refcounts, a channel or range is advertised to the mesh
  // when it gains its first local subscriber and withdrawn on its last.
  static std::unordered_map<uint64_t, unsigned int> _globalChannels;
  static std::map<ChannelRange, unsigned int> _globalRanges;

  // Dispatch indexes, points in a hash map, ranges in a flat vector,
  // matching is exact.
  static std::unordered_map<
      uint64_t, std::unordered_set<std::shared_ptr<ChannelSubscriber>>>
      _channelIndex;
  struct LocalRange {
    uint64_t lo;
    uint64_t hi;
    std::shared_ptr<ChannelSubscriber> sub;
  };
  static std::vector<LocalRange> _rangeIndex;

  // Channels this ChannelSubscriber is listening to. Hot-path membership
  // check for every delivered message, hence unordered_set.
  std::unordered_set<uint64_t> _localChannels;
  std::vector<ChannelRange> _localRanges;
};

}  // namespace Ardos

#endif  // ARDOS_CHANNEL_SUBSCRIBER_H
