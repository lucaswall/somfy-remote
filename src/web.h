#pragma once

#include <ESP8266WebServer.h>

#include "ha_mqtt.h"
#include "net.h"
#include "receiver.h"
#include "remotes.h"
#include "store.h"

// The device's own page: state, the two log rings, and the only place configuration is
// edited.
//
// The synchronous server from the core, not an async one: the payload is under a kilobyte
// and the radio already blocks for most of a second on every press.
//
// **Three pages, one boundary.** `/` is operation — exactly what Home Assistant exposes, so
// it needs no password; a page that asks for one before saying whether a shutter is shut is
// a page nobody opens. `/settings` is administration, and the boundary is real: Prog enrols
// a remote at a motor and removing one takes three entities with it.
//
// `/controls` is the tool for teaching the bridge which physical handhelds exist. A separate
// page because it is used differently — walking a house with a phone — and behind the
// password for a different reason: the addresses on it are the credentials of real motors.
class WebUi {
 public:
  WebUi(Remotes &remotes, Store &store, HaMqtt &mqtt, Net &net, Receiver &receiver,
        const char *hostname)
      : _server(80), _remotes(remotes), _store(store), _mqtt(mqtt), _net(net),
        _receiver(receiver), _hostname(hostname) {}

  // Starts itself once WiFi is up, the same way OTA does, so main does not have to
  // sequence them.
  void loop();

 private:
  void start();
  bool settingsAuthorised();
  bool sameOrigin();
  void handleState();
  void handleSend();
  void handleLog();
  void handleErrors();
  void handleStatus();
  void handleSettings();
  void handleProg();
  void handleRemoteAdd();
  void handleRemoteRemove();
  void handleRemoteFlags();
  void handleControls();
  void handleHeard();
  void handleControlSave();
  void handleControlForget();
  void handleControlIgnore();
  void handleCapture();
  void handleLearn();

  bool _started = false;

  ESP8266WebServer _server;
  Remotes &_remotes;
  Store &_store;
  HaMqtt &_mqtt;
  Net &_net;
  Receiver &_receiver;
  const char *_hostname;
};
