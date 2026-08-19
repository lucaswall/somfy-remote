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
//
// `/controls` sits behind the same password and is the tool for teaching the bridge which
// physical handhelds and wall buttons exist. A separate page rather than a section of
// settings because it is used differently: walking around a house with a phone, pressing a
// button and naming whatever appears. It needs the password for a different reason too —
// the addresses on it are the RF credentials of the motors in this house, and they must not
// reach `/api/state`, which both open pages already poll.
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
  void handleArm();
  void handleControlSave();
  void handleControlForget();
  void handleControlIgnore();
  void handleCapture();

  bool _started = false;

  ESP8266WebServer _server;
  Remotes &_remotes;
  Store &_store;
  HaMqtt &_mqtt;
  Net &_net;
  Receiver &_receiver;
  const char *_hostname;
};
