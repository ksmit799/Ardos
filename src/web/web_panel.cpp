#include "web_panel.h"

#include "../net/datagram.h"
#include "../util/config.h"
#include "../util/globals.h"
#include "../util/logger.h"

namespace Ardos {

WebPanel::WebPanel() {
  Logger::Info("Starting Web Panel component...");

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
      });

  _server->SetClientDisconnectedCallback([](ws28::Client *client) {
    Logger::Verbose(
        std::format("[WEB] Client '{}' disconnected", client->GetIP()));
  });

  _server->SetClientDataCallback(
      [](ws28::Client *client, char *data, size_t len, int opcode) {
        client->Send(data, len, opcode);
      });

  // Start listening!
  _server->Listen(_port);

  Logger::Info(std::format("[WEB] Listening on {}", _port));
}

} // namespace Ardos