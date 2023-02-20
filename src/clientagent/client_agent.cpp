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

  // Server version configuration.
  _version = config["version"].as<std::string>();

  // DC hash configuration.
  // Can be manually overriden in CA config.
  _dcHash = g_dc_file->get_hash();
  if (auto manualHash = config["manual-dc-hash"]) {
    _dcHash = manualHash.as<uint32_t>();
  }

  // Heartbeat interval configuration.
  // By default, heartbeats are disabled.
  _heartbeatInterval = 0;
  if (auto heartbeatParam = config["heartbeat-interval"]) {
    _heartbeatInterval = heartbeatParam.as<long>();
  }

  // Auth timeout configuration.
  // By default, auth timeout is disabled.
  _authTimeout = 0;
  if (auto timeoutParam = config["auth-timeout"]) {
    _authTimeout = timeoutParam.as<long>();
  }

  // UberDOG's configuration.
  auto uberdogs = Config::Instance()->GetNode("uberdogs");
  for (auto uberdog : uberdogs) {
    DCClass *dcc =
        g_dc_file->get_class_by_name(uberdog["class"].as<std::string>());
    if (!dcc) {
      Logger::Error(std::format(
          "[CA] UberDOG: {} Distributed Class: {} does not exist!",
          uberdog["id"].as<uint32_t>(), uberdog["class"].as<std::string>()));
      exit(1);
    }

    Uberdog ud{};
    ud.doId = uberdog["id"].as<uint32_t>();
    ud.dcc = dcc;

    // UberDOG's are anonymous by default (non-authed CA's can't update fields
    // on them.)
    ud.anonymous = false;
    if (auto anonParam = uberdog["anonymous"]) {
      ud.anonymous = anonParam.as<bool>();
    }

    _uberdogs[ud.doId] = ud;
  }

  // Owned objects relocation configuration.
  _relocateAllowed = true;
  if (auto relocateParam = config["relocate-allowed"]) {
    _relocateAllowed = relocateParam.as<bool>();
  }

  // Interests permission level configuration.
  _interestsPermission = INTERESTS_DISABLED;
  if (auto interestParam = config["interests"]) {
    auto level = interestParam.as<std::string>();
    if (level == "enabled") {
      _interestsPermission = INTERESTS_ENABLED;
    } else if (level == "visible") {
      _interestsPermission = INTERESTS_VISIBLE;
    }
  }

  // Interest operation timeout config.
  _interestTimeout = 500;
  if (auto timeoutParam = config["interest-timeout"]) {
    _interestTimeout = timeoutParam.as<unsigned long>();
  }

  // Channel allocation configuration.
  auto channelsParam = config["channels"];
  _nextChannel = channelsParam["min"].as<uint64_t>();
  _channelsMax = channelsParam["max"].as<uint64_t>();

  // UberDOG auth shim.
  // This allows clients authenticating over Disney specific login methods to
  // authenticate with the cluster.
  if (auto shimParam = config["ud-auth-shim"]) {
    _udAuthShim = shimParam.as<uint32_t>();
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

  Logger::Info(std::format("[CA] Listening on {}:{}", _host, _port));
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
 * Returns the DoId of the configured UD Authentication Shim (or 0 if none
 * is configured).
 * @return
 */
uint32_t ClientAgent::GetAuthShim() const { return _udAuthShim; }

/**
 * Returns the configured server version.
 * @return
 */
std::string ClientAgent::GetVersion() const { return _version; }

/**
 * Returns the computed DC hash or a configured override.
 * @return
 */
uint32_t ClientAgent::GetHash() const { return _dcHash; }

/**
 * Returns the expected client heartbeat interval.
 * @return
 */
unsigned long ClientAgent::GetHeartbeatInterval() const {
  return _heartbeatInterval;
}

/**
 * Returns the number of MS a client is expected to auth within.
 * @return
 */
unsigned long ClientAgent::GetAuthTimeout() const { return _authTimeout; }

/**
 * Returns the configured UberDOG's.
 * @return
 */
std::unordered_map<uint32_t, Uberdog> ClientAgent::Uberdogs() const {
  return _uberdogs;
}

/**
 * Returns whether or not clients are allowed to relocate the location of
 * objects they have ownership of.
 * @return
 */
bool ClientAgent::GetRelocateAllowed() const { return _relocateAllowed; }

/**
 * Returns the permission level of clients setting their own interests.
 * Options are: Enabled, Visible only (they must have visibility of the parent),
 * Disabled.
 * @return
 */
InterestsPermission ClientAgent::GetInterestsPermission() const {
  return _interestsPermission;
}

/**
 * Returns the number of MS an interest operation can run for before timing out.
 * @return
 */
unsigned long ClientAgent::GetInterestTimeout() const {
  return _interestTimeout;
}

} // namespace Ardos
