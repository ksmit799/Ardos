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

class ClientAgent {
public:
  ClientAgent();

  uint64_t AllocateChannel();
  void FreeChannel(const uint64_t &channel);

  [[nodiscard]] uint32_t GetAuthShim() const;
  [[nodiscard]] std::string GetVersion() const;
  [[nodiscard]] uint32_t GetHash() const;
  [[nodiscard]] long GetHeartbeatInterval() const;
  [[nodiscard]] long GetAuthTimeout() const;
  [[nodiscard]] std::unordered_map<uint32_t, Uberdog> Uberdogs() const;

private:
  std::shared_ptr<uvw::TCPHandle> _listenHandle;

  std::string _host = "127.0.0.1";
  int _port = 6667;

  std::string _version;
  uint32_t _dcHash;
  long _heartbeatInterval;
  long _authTimeout;

  std::unordered_map<uint32_t, Uberdog> _uberdogs;

  uint64_t _nextChannel;
  uint64_t _channelsMax;
  std::queue<uint64_t> _freedChannels;

  uint32_t _udAuthShim = 0;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_AGENT_H
