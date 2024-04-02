#ifndef ARDOS_WEB_PANEL_H
#define ARDOS_WEB_PANEL_H

#include <string>

#include <nlohmann/json.hpp>

#include "../net/ws/Server.h"

namespace Ardos {

class WebPanel {
public:
  WebPanel();

  static WebPanel *Instance;

  typedef struct {
    bool authed;
  } ClientData;

private:
  static void Send(ws28::Client *client, const nlohmann::json &data);
  void HandleData(ws28::Client *client, const std::string &data);

  std::string _name = "Ardos";
  int _port = 7781;

  std::string _username = "ardos";
  std::string _password = "ardos";

  std::string _cert;
  std::string _key;

  bool _secure = false;

  std::unique_ptr<ws28::Server> _server;
};

} // namespace Ardos

#endif // ARDOS_WEB_PANEL_H
