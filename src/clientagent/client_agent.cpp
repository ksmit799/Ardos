#include "client_agent.h"

#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"
#include "../util/metrics.h"
#include "client_participant.h"

namespace Ardos {

ClientAgent::ClientAgent() {
  Logger::Info("Starting Client Agent component...");

  _listenHandle = g_loop->resource<uvw::tcp_handle>();

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
  if (auto authShimParam = config["ud-auth-shim"]) {
    _udAuthShim = authShimParam.as<uint32_t>();
  }

  // UberDOG chat shim.
  // This allows clients sending chat messages with an unmodified Disney otp.dc
  // to have filtering done on an UberDOG (as opposed to none at all.)
  if (auto chatShimParam = config["ud-chat-shim"]) {
    _udChatShim = chatShimParam.as<uint32_t>();
  }

  // Socket events.
  _listenHandle->on<uvw::listen_event>(
      [this](const uvw::listen_event &, uvw::tcp_handle &srv) {
        std::shared_ptr<uvw::tcp_handle> client =
            srv.parent().resource<uvw::tcp_handle>();
        srv.accept(*client);

        // Create a new client for this connected participant.
        // TODO: These should be tracked in a vector.
        new ClientParticipant(this, client);
      });

  // Initialize metrics.
  InitMetrics();

  // Start listening!
  _listenHandle->bind(_host, _port);
  _listenHandle->listen();

  Logger::Info(std::format("[CA] Listening on {}:{}", _host, _port));
}

/**
 * Allocates a new channel to be used by a connected client within this CA's
 * allocation range.
 * @return
 */
uint64_t ClientAgent::AllocateChannel() {
  if (_nextChannel <= _channelsMax) {
    if (_freeChannelsGauge) {
      _freeChannelsGauge->Decrement();
    }

    return _nextChannel++;
  } else if (!_freedChannels.empty()) {
    if (_freeChannelsGauge) {
      _freeChannelsGauge->Decrement();
    }

    uint64_t channel = _freedChannels.front();
    _freedChannels.pop();
    return channel;
  }

  return 0;
}

/**
 * Free's a previously allocated channel to be re-used.
 * @param channel
 */
void ClientAgent::FreeChannel(const uint64_t &channel) {
  _freedChannels.push(channel);

  if (_freeChannelsGauge) {
    _freeChannelsGauge->Increment();
  }
}

/**
 * Returns the DoId of the configured UD Authentication Shim (or 0 if none
 * is configured).
 * @return
 */
uint32_t ClientAgent::GetAuthShim() const { return _udAuthShim; }

/**
 * Returns the DoId of the configured UD Chat Shim (or 0 if none
 * is configured).
 * @return
 */
uint32_t ClientAgent::GetChatShim() const { return _udChatShim; }

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

/**
 * Called when a participant connects.
 */
void ClientAgent::ParticipantJoined() {
  if (_participantsGauge) {
    _participantsGauge->Increment();
  }
}

/**
 * Called when a participant disconnects.
 */
void ClientAgent::ParticipantLeft() {
  if (_participantsGauge) {
    _participantsGauge->Decrement();
  }
}

/**
 * Records a handled datagram by a connected client.
 */
void ClientAgent::RecordDatagram(const uint16_t &size) {
  if (_datagramsProcessedCounter) {
    _datagramsProcessedCounter->Increment();
  }

  if (_datagramsSizeHistogram) {
    _datagramsSizeHistogram->Observe((double)size);
  }
}

/**
 * Records a timed out interest operation.
 */
void ClientAgent::RecordInterestTimeout() {
  if (_interestsTimeoutCounter) {
    _interestsTimeoutCounter->Increment();
  }
}

/**
 * Records the time taken for an interest operation to complete.
 * @param seconds
 */
void ClientAgent::RecordInterestTime(const double &seconds) {
  if (_interestsTimeHistogram) {
    _interestsTimeHistogram->Observe(seconds);
  }
}

/**
 * Initializes metrics collection for the client agent.
 */
void ClientAgent::InitMetrics() {
  // Make sure we want to collect metrics on this cluster.
  if (!Metrics::Instance()->WantMetrics()) {
    return;
  }

  auto registry = Metrics::Instance()->GetRegistry();

  auto &datagramsBuilder = prometheus::BuildCounter()
                               .Name("ca_handled_datagrams_total")
                               .Help("Number of datagrams handled")
                               .Register(*registry);

  auto &datagramsSizeBuilder = prometheus::BuildHistogram()
                                   .Name("ca_datagrams_bytes_size")
                                   .Help("Bytes size of handled datagrams")
                                   .Register(*registry);

  auto &participantsBuilder = prometheus::BuildGauge()
                                  .Name("ca_participants_size")
                                  .Help("Number of connected participants")
                                  .Register(*registry);

  auto &freeChannelsBuilder = prometheus::BuildGauge()
                                  .Name("ca_free_channels_size")
                                  .Help("Number of free channels")
                                  .Register(*registry);

  auto &timeoutsBuilder = prometheus::BuildCounter()
                              .Name("ca_interests_timeout_total")
                              .Help("Number of interest timeouts")
                              .Register(*registry);

  auto &interestsTimeBuilder =
      prometheus::BuildHistogram()
          .Name("ca_interests_time_seconds")
          .Help("Time to complete an interest operation")
          .Register(*registry);

  _datagramsProcessedCounter = &datagramsBuilder.Add({});
  _datagramsSizeHistogram = &datagramsSizeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{1, 4, 16, 64, 256, 1024, 4096,
                                                  16384, 65536});
  _participantsGauge = &participantsBuilder.Add({});
  _freeChannelsGauge = &freeChannelsBuilder.Add({});
  _interestsTimeoutCounter = &timeoutsBuilder.Add({});
  _interestsTimeHistogram = &interestsTimeBuilder.Add(
      {}, prometheus::Histogram::BucketBoundaries{0, 0.5, 1, 1.5, 2, 2.5, 3,
                                                  3.5, 4, 4.5, 5});

  // Initialize free channels to our range of allocated channels.
  _freeChannelsGauge->Set((double)(_channelsMax - _nextChannel));
}

void ClientAgent::HandleWeb(ws28::Client *client, nlohmann::json &data) {}

} // namespace Ardos
