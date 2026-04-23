#ifndef ARDOS_CLIENT_AGENT_H
#define ARDOS_CLIENT_AGENT_H

#include <dcClass.h>
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/histogram.h>
#include <ws28/Client.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <queue>
#include <unordered_set>
#include <utility>
#include <uvw.hpp>
#include <vector>

namespace Ardos {

struct Uberdog {
  uint32_t doId;
  DCClass* dcc;
  bool anonymous;
};

enum InterestsPermission {
  INTERESTS_ENABLED,
  INTERESTS_VISIBLE,
  INTERESTS_DISABLED,
};

// Zone filter mode when interest.zones is configured (omitted = no filter).
enum InterestMode {
  INTEREST_MODE_NONE,
  INTEREST_MODE_WHITELIST,
  INTEREST_MODE_BLACKLIST,
};

class ClientParticipant;

class ClientAgent {
 public:
  ClientAgent();

  uint64_t AllocateChannel();
  void FreeChannel(const uint64_t& channel);

  [[nodiscard]] uint32_t GetAuthShim() const;
  [[nodiscard]] uint32_t GetChatShim() const;
  [[nodiscard]] std::string GetVersion() const;
  [[nodiscard]] uint32_t GetHash() const;
  [[nodiscard]] unsigned long GetHeartbeatInterval() const;
  [[nodiscard]] unsigned long GetAuthTimeout() const;
  [[nodiscard]] unsigned long GetHistoricalTTL() const;
  [[nodiscard]] std::unordered_map<uint32_t, Uberdog> Uberdogs() const;
  [[nodiscard]] bool GetRelocateAllowed() const;
  [[nodiscard]] InterestsPermission GetInterestsPermission() const;
  [[nodiscard]] InterestMode GetInterestMode() const;
  [[nodiscard]] const std::unordered_set<uint32_t>& GetInterestZones() const;
  [[nodiscard]] const std::vector<std::pair<uint32_t, uint32_t>>&
  GetInterestZoneRanges() const;
  [[nodiscard]] unsigned long GetInterestTimeout() const;
  [[nodiscard]] DCClass* GetAvatarClass() const;
  [[nodiscard]] bool GetParentingRulesEnabled() const;

  void ParticipantJoined();
  void ParticipantLeft(ClientParticipant* client);
  void RecordDatagram(const uint16_t& size);
  void RecordInterestTimeout();
  void RecordInterestTime(const double& seconds);

  void HandleWeb(ws28::Client* client, nlohmann::json& data);

 private:
  void InitMetrics();

  std::shared_ptr<uvw::tcp_handle> _listenHandle;

  std::string _host = "127.0.0.1";
  int _port = 6667;

  std::string _version;
  uint32_t _dcHash;
  unsigned long _heartbeatInterval;
  unsigned long _authTimeout;
  unsigned long _historicalTTL;
  bool _relocateAllowed;
  InterestsPermission _interestsPermission;
  InterestMode _interestMode;
  std::unordered_set<uint32_t> _interestZones;
  std::vector<std::pair<uint32_t, uint32_t>> _interestZoneRanges;
  unsigned long _interestTimeout;
  bool _parentingRulesEnabled = true;

  std::unordered_map<uint32_t, Uberdog> _uberdogs;
  DCClass* _avatarClass = nullptr;

  std::unordered_set<ClientParticipant*> _participants;

  uint64_t _nextChannel;
  uint64_t _channelsMax;
  std::queue<uint64_t> _freedChannels;

  uint32_t _udAuthShim = 0;
  uint32_t _udChatShim = 0;

  prometheus::Counter* _datagramsProcessedCounter = nullptr;
  prometheus::Histogram* _datagramsSizeHistogram = nullptr;
  prometheus::Gauge* _participantsGauge = nullptr;
  prometheus::Gauge* _freeChannelsGauge = nullptr;
  prometheus::Counter* _interestsTimeoutCounter = nullptr;
  prometheus::Histogram* _interestsTimeHistogram = nullptr;
};

}  // namespace Ardos

#endif  // ARDOS_CLIENT_AGENT_H
