#include "web_panel.h"

#include <nlohmann/json.hpp>

#include "../net/datagram.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

WebPanel *WebPanel::Instance = nullptr;

WebPanel::WebPanel() {
  Logger::Info("Starting Web Panel component...");

  Instance = this;

  // Web Panel configuration.
  auto config = Config::Instance()->GetNode("web-panel");

  // Login configuration.
  if (auto userParam = config["username"]) {
    _username = userParam.as<std::string>();
  }
  if (auto passParam = config["password"]) {
    _password = passParam.as<std::string>();
  }

  // Port configuration.
  if (auto portParam = config["port"]) {
    _port = portParam.as<int>();
  }

  _server = new ws28::Server(g_loop->raw());

  // Set a max message size that reflects the
  // max length of a Datagram (+2 for length header.)
  _server->SetMaxMessageSize(kMaxDgSize + 2);

  _server->SetClientConnectedCallback(
      [](ws28::Client *client, ws28::HTTPRequest &) {
        Logger::Verbose(
            std::format("[WEB] Client connected from {}", client->GetIP()));

        auto *data = (ClientData *)malloc(sizeof(ClientData));
        data->authed = false;

        client->SetUserData(data);
      });

  _server->SetClientDisconnectedCallback([](ws28::Client *client) {
    Logger::Verbose(
        std::format("[WEB] Client '{}' disconnected", client->GetIP()));

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

  Logger::Info(std::format("[WEB] Listening on {}", _port));
}

void WebPanel::HandleData(ws28::Client *client, const std::string &data) {
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
      client->Close(401, "Invalid auth credentials");
      return;
    }

    clientData->authed = true;
  } else {
    client->Send(data.c_str(), data.length(), 1);
  }
}

} // namespace Ardos