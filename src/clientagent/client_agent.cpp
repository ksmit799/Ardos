#include "client_agent.h"

#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "client_participant.h"

namespace Ardos {

ClientAgent::ClientAgent() {
  Logger::Info("Starting Client Agent component...");

  _listenHandle = g_loop->resource<uvw::TCPHandle>();

  auto config = Config::Instance()->GetNode("client-agent");

  // Listen configuration.
  if (auto hostParam = config["host"]) {
    _host = hostParam.as<std::string>();
  }
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }

  // Channel allocation configuration.
  auto channelsParam = config["channels"];
  _nextChannel = channelsParam["min"].as<uint64_t>();
  _channelsMax = channelsParam["max"].as<uint64_t>();

  // UberDOG auth shim.
  // This allows clients authenticating over Disney specific login methods to
  // authenticate with the cluster.
  if (auto shimParam = config["ud-auth-shim"]) {
    _udAuthShim = shimParam.as<uint64_t>();
  }

  // Socket events.
  _listenHandle->on<uvw::ListenEvent>(
      [this](const uvw::ListenEvent &, uvw::TCPHandle &srv) {
        std::shared_ptr<uvw::TCPHandle> client =
            srv.loop().resource<uvw::TCPHandle>();
        srv.accept(*client);

        // Create a new client for this connected participant.
        // TODO: These should be tracked in a vector.
        new ClientParticipant(this, client);
      });

  // Start listening!
  _listenHandle->bind(_host, _port);
}

/**
 * Allocates a new channel to be used by a connected client within this CA's
 * allocation range.
 * @return
 */
uint64_t ClientAgent::AllocateChannel() {
  if (_nextChannel <= _channelsMax) {
    return _nextChannel++;
  } else {
    if (!_freedChannels.empty()) {
      uint64_t channel = _freedChannels.front();
      _freedChannels.pop();
      return channel;
    }
  }

  return 0;
}

/**
 * Free's a previously allocated channel to be re-used.
 * @param channel
 */
void ClientAgent::FreeChannel(const uint64_t &channel) {
  _freedChannels.push(channel);
}

/**
 * Returns the channel ID of the configured UD Authentication Shim (or 0 if none
 * is configured).
 * @return
 */
uint64_t ClientAgent::GetAuthShim() { return _udAuthShim; }

} // namespace Ardos
