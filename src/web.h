#pragma once

#include <ESP8266WebServer.h>

#include "ha_mqtt.h"
#include "net.h"
#include "remotes.h"
#include "store.h"

// The device's own page: state, the two log rings, and the only place configuration is
// edited.
//
// Deliberately the synchronous server bundled with the core rather than an async one. The
// payload is under a kilobyte, the radio blocks for most of a second on every press, and a
// request that arrives mid-transmission simply waits — none of which argues for a second
// TCP stack.
//
// **Two pages, one boundary.** `/` is operation — up, stop, down, and state. It is exactly
// what Home Assistant already exposes, so it is open: a page that asks for a password
// before it will say whether a shutter is shut is a page nobody opens.
//
// `/settings` is administration — pairing, adding and removing remotes — and the browser
// asks for a password when it is opened. That is a real boundary rather than decoration:
// Prog enrols an emulated remote at a motor, and removing one takes three Home Assistant
// entities with it.
class WebUi {
 public:
  WebUi(Remotes &remotes, Store &store, HaMqtt &mqtt, Net &net, const char *hostname)
      : _server(80), _remotes(remotes), _store(store), _mqtt(mqtt), _net(net),
        _hostname(hostname) {}

  // Starts itself once WiFi is up, the same way OTA does, so main does not have to
  // sequence them.
  void loop();

 private:
  void start();
  bool settingsAuthorised();
  void handleState();
  void handleSend();
  void handleLog();
  void handleErrors();
  void handleStatus();
  void handleSettings();
  void handleProg();
  void handleRemoteAdd();
  void handleRemoteRemove();

  bool _started = false;

  ESP8266WebServer _server;
  Remotes &_remotes;
  Store &_store;
  HaMqtt &_mqtt;
  Net &_net;
  const char *_hostname;
};
