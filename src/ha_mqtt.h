#pragma once

#include <PubSubClient.h>
#include <WiFiClient.h>

#include "remotes.h"
#include "rolling_code.h"

// The only place that knows Home Assistant exists. Remotes speaks presses and shutters;
// everything HA-shaped — discovery payloads, topic names, the switch that has to report
// itself back off — stops here.
//
// Each emulated remote becomes one Home Assistant device carrying three entities: a cover
// for up/stop/down, a switch for My because Google Home will not surface a button, and a
// button for Prog.
class HaMqtt {
 public:
  HaMqtt(Remotes &remotes, const char *clientId)
      : _mqtt(_wifi), _remotes(remotes), _clientId(clientId) {}

  void loop();
  bool connected() { return _mqtt.connected(); }

 private:
  bool connect();
  void publishDiscovery(uint8_t remote);
  void publishState(uint8_t remote);
  void onMessage(const char *topic, const uint8_t *payload, unsigned int length);

  WiFiClient _wifi;
  PubSubClient _mqtt;
  Remotes &_remotes;
  const char *_clientId;

  uint32_t _publishedVersion[ROLLING_CODE_MAX_REMOTES] = {0};
  uint32_t _lastAttempt = 0;
  uint32_t _retryMs = 5000;
  bool _attempted = false;
};
