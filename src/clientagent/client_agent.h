#ifndef ARDOS_CLIENT_AGENT_H
#define ARDOS_CLIENT_AGENT_H

#include <memory>
#include <queue>

#include <dcClass.h>
#include <uvw.hpp>

namespace Ardos {

struct Uberdog {
  uint32_t doId;
  DCClass *dcc;
  bool anonymous;
};

enum InterestsPermission {
  INTERESTS_ENABLED,
  INTERESTS_VISIBLE,
  INTERESTS_DISABLED,
};

class ClientAgent {
public:
  ClientAgent();

  uint64_t AllocateChannel();
  void FreeChannel(const uint64_t &channel);

  [[nodiscard]] uint32_t GetAuthShim() const;
  [[nodiscard]] std::string GetVersion() const;
  [[nodiscard]] uint32_t GetHash() const;
  [[nodiscard]] unsigned long GetHeartbeatInterval() const;
  [[nodiscard]] unsigned long GetAuthTimeout() const;
  [[nodiscard]] std::unordered_map<uint32_t, Uberdog> Uberdogs() const;
  [[nodiscard]] bool GetRelocateAllowed() const;
  [[nodiscard]] InterestsPermission GetInterestsPermission() const;
  [[nodiscard]] unsigned long GetInterestTimeout() const;

private:
  std::shared_ptr<uvw::TCPHandle> _listenHandle;

  std::string _host = "127.0.0.1";
  int _port = 6667;

  std::string _version;
  uint32_t _dcHash;
  unsigned long _heartbeatInterval;
  unsigned long _authTimeout;
  bool _relocateAllowed;
  InterestsPermission _interestsPermission;
  unsigned long _interestTimeout;

  std::unordered_map<uint32_t, Uberdog> _uberdogs;

  uint64_t _nextChannel;
  uint64_t _channelsMax;
  std::queue<uint64_t> _freedChannels;

  uint32_t _udAuthShim = 0;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_AGENT_H
