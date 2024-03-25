#ifndef ARDOS_WEB_PANEL_H
#define ARDOS_WEB_PANEL_H

#include <string>

#include "../net/ws/Server.h"

namespace Ardos {

class WebPanel {
public:
  WebPanel();

private:
  std::string _username = "ardos";
  std::string _password = "ardos";
  int _port = 7781;

  ws28::Server *_server;
};

} // namespace Ardos

#endif // ARDOS_WEB_PANEL_H
