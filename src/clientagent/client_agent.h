#ifndef ARDOS_CLIENT_AGENT_H
#define ARDOS_CLIENT_AGENT_H

#include <memory>
#include <queue>

#include <uvw.hpp>

namespace Ardos {

class ClientAgent {
public:
  ClientAgent();

  uint64_t AllocateChannel();
  void FreeChannel(const uint64_t &channel);

  uint64_t GetAuthShim();

private:
  std::shared_ptr<uvw::TCPHandle> _listenHandle;

  std::string _host = "127.0.0.1";
  int _port = 6667;

  uint64_t _nextChannel;
  uint64_t _channelsMax;
  std::queue<uint64_t> _freedChannels;

  uint64_t _udAuthShim = 0;
};

} // namespace Ardos

#endif // ARDOS_CLIENT_AGENT_H
