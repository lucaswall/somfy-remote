#include "net.h"

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <ESP8266WiFi.h>

#include "log.h"
#include "timing.h"

static const uint32_t CONNECT_TIMEOUT_MS = 20000;

void Net::begin(const char *hostname, const char *ssid, const char *password,
                const char *otaPassword) {
  _hostname = hostname;
  _otaPassword = otaPassword;

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);   // credentials come from secrets.h; rewriting flash every boot
                            // only wears it out
  WiFi.setAutoReconnect(true);
  WiFi.hostname(hostname);
  WiFi.begin(ssid, password);

  logLine("wifi      : connecting to %s", ssid);
  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && !elapsed(millis(), start, CONNECT_TIMEOUT_MS)) {
    delay(200);
  }

  // Not fatal: the SDK keeps retrying, so a slow AP resolves itself without a reboot.
  if (WiFi.status() != WL_CONNECTED) {
    logError("wifi      : not up yet (status %d), retrying in background", WiFi.status());
  }
}

bool Net::connected() const { return WiFi.status() == WL_CONNECTED; }

// WiFi.RSSI() returns 31 when unassociated, which would be logged as a positive dBm.
int Net::rssi() const { return connected() ? WiFi.RSSI() : 0; }

void Net::loop() {
  const bool up = connected();

  if (up) {
    // Started here rather than in begin() so a board that joins late still becomes
    // reachable over the air without a reboot.
    if (!_otaReady) {
      ArduinoOTA.setHostname(_hostname);
      ArduinoOTA.setPassword(_otaPassword);
      ArduinoOTA.onStart([]() { logLine("ota       : start"); });
      ArduinoOTA.onEnd([]() { logLine("ota       : done, rebooting"); });
      ArduinoOTA.onError([](ota_error_t e) { logError("ota       : error %u", e); });
      ArduinoOTA.begin();
      _otaReady = true;
      logLine("ota       : ready on %s at %s", _hostname,
           WiFi.localIP().toString().c_str());
    }
    ArduinoOTA.handle();
  }

  if (up != _wasConnected) {
    _wasConnected = up;
    if (up) {
      logLine("wifi      : up, ip %s rssi %d", WiFi.localIP().toString().c_str(),
           WiFi.RSSI());
    } else {
      // Nothing to tear down: ArduinoOTA.begin() returns early once initialised and its
      // listener is bound to IP_ADDR_ANY, and MDNSResponder installs an interface-status
      // callback that restarts itself. Both come back with the association.
      logError("wifi      : lost");
    }
  }
}
