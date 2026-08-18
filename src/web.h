#pragma once

#include <ESP8266WebServer.h>

#include "net.h"
#include "remotes.h"

// Debug UI: one page, a state endpoint the browser polls, and one command endpoint.
//
// Deliberately the synchronous server bundled with the core rather than an async one. The
// payload is under a kilobyte, the radio blocks for most of a second on every press, and a
// request that arrives mid-transmission simply waits — none of which argues for a second
// TCP stack.
class WebUi {
 public:
  WebUi(Remotes &remotes, Net &net, const char *hostname)
      : _server(80), _remotes(remotes), _net(net), _hostname(hostname) {}

  // Starts itself once WiFi is up, the same way OTA does, so main does not have to
  // sequence them.
  void loop();

 private:
  void start();
  void handleState();
  void handleSend();
  void handleLog();
  void handleErrors();
  void handleStatus();

  bool _started = false;

  ESP8266WebServer _server;
  Remotes &_remotes;
  Net &_net;
  const char *_hostname;
};
