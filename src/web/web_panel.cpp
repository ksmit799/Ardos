#include "web_panel.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include "../clientagent/client_agent.h"
#include "../database/database_server.h"
#include "../messagedirector/message_director.h"
#include "../stateserver/database_state_server.h"
#include "../util/config.h"
#include "../util/logger.h"

namespace Ardos {

WebPanel *WebPanel::Instance = nullptr;

WebPanel::WebPanel() {
  spdlog::info("Starting Web Panel component...");

  Instance = this;

  // Web Panel configuration.
  auto config = Config::Instance()->GetNode("web-panel");

  // Log configuration.
  spdlog::stdout_color_mt("web");
  if (auto logLevel = config["log-level"]) {
    spdlog::get("web")->set_level(
        Logger::LevelFromString(logLevel.as<std::string>()));
  }

  // Cluster name configuration.
  if (auto nameParam = config["name"]) {
    _name = nameParam.as<std::string>();
  }
  // Port configuration.
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }

  // Login configuration.
  if (auto userParam = config["username"]) {
    _username = userParam.as<std::string>();
  }
  if (auto passParam = config["password"]) {
    _password = passParam.as<std::string>();
  }

  // SSL configuration.
  if (auto certParam = config["certificate"]) {
    _cert = certParam.as<std::string>();
  }
  if (auto keyParam = config["private-key"]) {
    _key = keyParam.as<std::string>();
  }

  if (!_cert.empty() && !_key.empty()) {
    // Configure SSL (if keys were supplied.)
    ws28::TLS::InitSSL();
    _secure = true;

    const SSL_METHOD *method = TLS_server_method();

    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
      spdlog::get("web")->error("Unable to create SSL context");
    }

    if (SSL_CTX_use_certificate_file(ctx, _cert.c_str(), SSL_FILETYPE_PEM) <=
        0) {
      spdlog::get("web")->error("Failed to load cert file: {}", _cert);
      exit(1);
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, _key.c_str(), SSL_FILETYPE_PEM) <= 0) {
      spdlog::get("web")->error("Failed to load private key file: {}", _cert);
      exit(1);
    }

    _server = std::make_unique<ws28::Server>(g_loop->raw(), ctx);
  } else {
    // Otherwise, create an unsecure server.
    _server = std::make_unique<ws28::Server>(g_loop->raw());
  }

  // Set a max message size that reflects the
  // max length of a Datagram (+2 for length header.)
  _server->SetMaxMessageSize(kMaxDgSize + 2);

  // Disable Origin checks.
  _server->SetCheckConnectionCallback(
      [](ws28::Client *client, ws28::HTTPRequest &) { return true; });

  _server->SetClientConnectedCallback(
      [](ws28::Client *client, ws28::HTTPRequest &) {
        spdlog::get("web")->debug("Client connected from {}", client->GetIP());

        auto *data = (ClientData *)malloc(sizeof(ClientData));
        data->authed = false;

        client->SetUserData(data);
      });

  _server->SetClientDisconnectedCallback([](ws28::Client *client) {
    spdlog::get("web")->debug("Client '{}' disconnected", client->GetIP());

    // Free alloc'd user data.
    if (client->GetUserData() != nullptr) {
      free(client->GetUserData());
      client->SetUserData(nullptr);
    }
  });

  _server->SetClientDataCallback(
      [](ws28::Client *client, char *data, size_t len, int opcode) {
        Instance->HandleData(client, {data, len});
      });

  // Start listening!
  _server->Listen(_port);

  spdlog::get("web")->info("Listening on {} [{}]", _port,
                           _secure ? "SECURE" : "UNSECURE");
}

void WebPanel::Send(ws28::Client *client, const nlohmann::json &data) {
  auto res = data.dump();
  client->Send(res.c_str(), res.length(), 1);
}

void WebPanel::HandleData(ws28::Client *client, const std::string &data) {
  // Make sure we have a valid JSON request.
  if (!nlohmann::json::accept(data)) {
    client->Close(400, "Improperly formatted request");
    return;
  }

  // Parse the request data and client data.
  nlohmann::json message = nlohmann::json::parse(data);
  auto clientData = (ClientData *)client->GetUserData();

  // Make sure the request is valid.
  if (!message.contains("type") || !message["type"].is_string()) {
    client->Close(400, "Improperly formatted request");
    return;
  }

  // Make sure the first message is authentication.
  auto messageType = message["type"].template get<std::string>();
  if (!clientData->authed && messageType != "auth") {
    client->Close(403, "First message was not auth");
    return;
  }

  if (messageType == "auth") {
    // Validate the auth message.
    if (!message.contains("username") || !message["username"].is_string() ||
        !message.contains("password") || !message["password"].is_string()) {
      client->Close(400, "Improperly formatted request");
      return;
    }

    // Validate the auth credentials.
    if (message["username"].template get<std::string>() != _username ||
        message["password"].template get<std::string>() != _password) {
      // Send the auth response.
      Send(client, {{"type", "auth"}, {"success", false}});
      client->Close(401, "Invalid auth credentials");
      return;
    }

    clientData->authed = true;

    // Send the auth response.
    Send(client, {
                     {"type", "auth"},
                     {"success", true},
                     {"name", _name},
                 });
  } else if (messageType == "md") {
    // Handle the request on the Message Director.
    MessageDirector::Instance()->HandleWeb(client, message);
  } else if (messageType == "ss") {
    // Handle the request on the State Server.
    auto ss = MessageDirector::Instance()->GetStateServer();
    if (ss) {
      ss->HandleWeb(client, message);
    } else {
      Send(client, {
                       {"type", "ss"},
                       {"success", false},
                   });
    }
  } else if (messageType == "ca") {
    // Handle the request on the Client Agent.
    auto ca = MessageDirector::Instance()->GetClientAgent();
    if (ca) {
      ca->HandleWeb(client, message);
    } else {
      Send(client, {
                       {"type", "ca"},
                       {"success", false},
                   });
    }
  } else if (messageType == "db") {
    // Handle the request on the Database Server.
    auto db = MessageDirector::Instance()->GetDbServer();
    if (db) {
      db->HandleWeb(client, message);
    } else {
      Send(client, {
                       {"type", "db"},
                       {"success", false},
                   });
    }
  } else if (messageType == "dbss") {
    // Handle the request on the Database State Server.
    auto dbss = MessageDirector::Instance()->GetDbStateServer();
    if (dbss) {
      dbss->HandleWeb(client, message);
    } else {
      Send(client, {
                       {"type", "dbss"},
                       {"success", false},
                   });
    }
  } else if (messageType == "config") {
    // Return the full config file this deployment has been loaded with.
    Send(client, {
                     {"type", "config"},
                     {"config", YAML::Dump(Config::Instance()->GetConfig())},
                 });
  }
}

} // namespace Ardos