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
// **Reads are open; writes are not.** Pressing a button, adding a remote and removing one
// all change something outside this box — a motor, or a dozen Home Assistant entities — so
// the mutating endpoints require the shared secret from include/secrets.h. Reading state
// stays open, because a page that needs a password to show whether a shutter is shut is a
// page nobody opens.
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
  bool authorised();
  void handleState();
  void handleSend();
  void handleLog();
  void handleErrors();
  void handleStatus();
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
