#include "web.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <stdlib.h>

#include "config_doc.h"
#include "log.h"
#include "page.h"
#include "secrets.h"
#include "somfy_frame.h"

void WebUi::loop() {
  if (!_started) {
    if (WiFi.status() == WL_CONNECTED) {
      start();
    }
    return;
  }
  _server.handleClient();
}

void WebUi::start() {
  _server.on("/", HTTP_GET, [this]() {
    _server.sendHeader("Cache-Control", "no-store");
    _server.send_P(200, "text/html", PAGE_HTML);
  });
  // Shared by both pages, and the only thing here worth letting a browser cache.
  _server.on("/style.css", HTTP_GET, [this]() {
    _server.sendHeader("Cache-Control", "max-age=86400");
    _server.send_P(200, "text/css", PAGE_CSS);
  });
  _server.on("/settings", HTTP_GET, [this]() { handleSettings(); });
  _server.on("/api/prog", HTTP_POST, [this]() { handleProg(); });
  _server.on("/api/state", HTTP_GET, [this]() { handleState(); });
  _server.on("/api/send", HTTP_POST, [this]() { handleSend(); });
  _server.on("/log", HTTP_GET, [this]() { handleLog(); });
  _server.on("/errors", HTTP_GET, [this]() { handleErrors(); });
  _server.on("/status", HTTP_GET, [this]() { handleStatus(); });
  _server.on("/api/remote/add", HTTP_POST, [this]() { handleRemoteAdd(); });
  _server.on("/api/remote/remove", HTTP_POST, [this]() { handleRemoteRemove(); });
  _server.onNotFound([this]() { _server.send(404, "text/plain", "not found"); });
  _server.begin();

  // ArduinoOTA already started mDNS under the same hostname; advertising the web service
  // is what makes the board show up as a browsable device rather than just a pingable one.
  MDNS.addService("http", "tcp", 80);

  _started = true;
  logLine("web       : http://%s.local/  (http://%s/)", _hostname,
          WiFi.localIP().toString().c_str());
}

// The gate on the settings page and everything it can do. Basic auth, so the browser puts
// up its own prompt when the page is opened and remembers it for the endpoints behind it.
//
// Plain HTTP on a home LAN: anyone who can read the traffic can read the password. It is
// not protection against somebody already on the network — it is the boundary between the
// page anybody in the house opens to close a shutter and the page that pairs motors.
bool WebUi::settingsAuthorised() {
  if (_server.authenticate(WEB_USER, WEB_PASSWORD)) {
    return true;
  }
  _server.requestAuthentication(BASIC_AUTH, "somfy-remote settings",
                                "authentication required\n");
  return false;
}

void WebUi::handleSettings() {
  if (!settingsAuthorised()) {
    return;
  }
  _server.sendHeader("Cache-Control", "no-store");
  _server.send_P(200, "text/html", SETTINGS_HTML);
}

// Streamed rather than assembled: the remote list grows with the installation, and the
// loop stack is 4 KB.
void WebUi::handleState() {
  char chunk[192];
  const uint32_t up = millis() / 1000;

  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", "");

  snprintf(chunk, sizeof(chunk),
           "{\"host\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"heap\":%u,"
           "\"pending\":%u,\"configured\":%s,\"held\":%s,\"degraded\":%s,"
           "\"store\":\"%c\",\"free\":%u,\"epoch\":%lu,\"remotes\":[",
           _hostname, WiFi.localIP().toString().c_str(), _net.rssi(), (unsigned long)up,
           ESP.getFreeHeap(), _remotes.pending(), _mqtt.configured() ? "true" : "false",
           _store.legacyHeld() ? "true" : "false", _store.degraded() ? "true" : "false",
           _store.activeName(), _store.freeSlots(),
           (unsigned long)_mqtt.config().epoch);
  _server.sendContent(chunk);

  // The name comes from Home Assistant and is shown, never used to key anything: every
  // internal path here is the index. A remote with no name published simply reads as its
  // index, which is what the page did before names existed.
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    const RemoteState &state = _remotes.state(i);
    const char *name = _mqtt.nameOf(i);
    snprintf(chunk, sizeof(chunk),
             "%s{\"n\":%u,\"name\":\"%s\",\"position\":\"%s\",\"code\":%lu,"
             "\"version\":%lu,\"enabled\":%s,\"operational\":%s,\"ready\":%s}",
             i == 0 ? "" : ",", i, name[0] != '\0' ? name : "",
             coverPositionName(state.position()), (unsigned long)_remotes.counter(i),
             (unsigned long)state.version(), _remotes.enabled(i) ? "true" : "false",
             _remotes.operational(i) ? "true" : "false",
             _remotes.transmittable(i) ? "true" : "false");
    _server.sendContent(chunk);
  }

  _server.sendContent("]}");
  _server.sendContent("");
}

// Open, like the page it serves: this is what Home Assistant already lets anyone in the
// house do. Prog is deliberately not reachable here — it lives behind the settings
// password, because it changes a pairing rather than a position.
void WebUi::handleSend() {
  const String number = _server.arg("remote");
  const String button = _server.arg("command");

  SomfyCommand command;
  if (!somfyCommandFromText(button.c_str(), button.length(), &command)) {
    _server.send(400, "text/plain", "unknown command\n");
    return;
  }
  if (command == SOMFY_PROG) {
    _server.send(403, "text/plain", "Prog is a settings operation\n");
    return;
  }

  // Parsed strictly, not with toInt(): that reads an absent or non-numeric argument as 0,
  // so a request naming no remote at all would move shutter 0. The command half is already
  // length-exact for the same reason — see include/somfy_frame.h.
  char *end = nullptr;
  const long remote = strtol(number.c_str(), &end, 10);
  if (number.length() == 0 || *end != '\0' || remote < 0 || remote >= _remotes.count()) {
    _server.send(404, "text/plain", "no such remote\n");
    return;
  }

  // Answered here rather than at the radio, so Home Assistant and the page give the same
  // reason. A remote with no rolling code is the case that matters: transmitting without
  // one would desynchronise the motor.
  if (!_remotes.transmittable((uint8_t)remote)) {
    _server.send(409, "text/plain",
                 !_remotes.hasCounter((uint8_t)remote)
                     ? "remote has no rolling code yet\n"
                     : "remote is disabled or not operational\n");
    return;
  }

  _remotes.queue((uint8_t)remote, command);
  handleState();
}

// Behind the settings password. Held down at the motor, Prog enrols or drops this emulated
// remote — the one press here that changes something permanent rather than something that
// can be pressed back.
void WebUi::handleProg() {
  if (!settingsAuthorised()) {
    return;
  }
  const String number = _server.arg("remote");
  char *end = nullptr;
  const long remote = strtol(number.c_str(), &end, 10);
  if (number.length() == 0 || *end != '\0' || remote < 0 || remote >= _remotes.count()) {
    _server.send(404, "text/plain", "no such remote\n");
    return;
  }
  if (!_remotes.transmittable((uint8_t)remote)) {
    _server.send(409, "text/plain",
                 !_remotes.hasCounter((uint8_t)remote)
                     ? "remote has no rolling code yet\n"
                     : "remote is disabled or not operational\n");
    return;
  }
  _remotes.queue((uint8_t)remote, SOMFY_PROG);
  _server.send(200, "text/plain", "Prog queued\n");
}

// Adding takes the next never-used index, never a freed one. A recycled index would point
// an existing Home Assistant entity and an existing rolling code at different hardware —
// the counter belongs to the pair (index, address), and reusing one breaks that binding.
void WebUi::handleRemoteAdd() {
  if (!settingsAuthorised()) {
    return;
  }
  cfg::ConfigDoc next = _mqtt.config();
  if (next.entries >= rs::MAX_REMOTES) {
    _server.send(409, "text/plain", "no rolling code slots left\n");
    return;
  }

  const uint8_t index = next.remoteCount();
  if (index >= rs::MAX_REMOTES) {
    _server.send(409, "text/plain", "no indices left\n");
    return;
  }

  const String addr = _server.arg("address");
  next.remotes[next.entries].index = index;
  next.remotes[next.entries].address =
      addr.length() > 0 ? cfg::parseHex(addr.c_str(), rs::ADDR_NONE) : rs::ADDR_NONE;
  next.remotes[next.entries].enabled = true;
  // A new remote is not operational until somebody has paired it and seen it move. The
  // safe default is the one that cannot fire a motor by accident.
  next.remotes[next.entries].operational = false;
  next.entries++;

  if (!_mqtt.applyConfig(next, "ui")) {
    _server.send(500, "text/plain", "could not persist configuration\n");
    return;
  }
  char body[64];
  snprintf(body, sizeof(body), "added remote %u\n", index);
  _server.send(200, "text/plain", body);
}

// Removing disables in place and keeps the index reserved. Renumbering the survivors would
// re-key every Home Assistant entity above it, and deleting the rolling code would restart
// the counter at zero if the same shutter were ever added back.
void WebUi::handleRemoteRemove() {
  if (!settingsAuthorised()) {
    return;
  }
  const String number = _server.arg("remote");
  char *end = nullptr;
  const long remote = strtol(number.c_str(), &end, 10);
  if (number.length() == 0 || *end != '\0' || remote < 0 || remote >= _remotes.count()) {
    _server.send(404, "text/plain", "no such remote\n");
    return;
  }

  cfg::ConfigDoc next = _mqtt.config();
  bool found = false;
  for (uint8_t i = 0; i < next.entries; i++) {
    if (next.remotes[i].index == (uint8_t)remote) {
      next.remotes[i].enabled = false;
      next.remotes[i].operational = false;
      found = true;
    }
  }
  if (!found) {
    _server.send(404, "text/plain", "no such remote\n");
    return;
  }

  if (!_mqtt.applyConfig(next, "ui")) {
    _server.send(500, "text/plain", "could not persist configuration\n");
    return;
  }
  _mqtt.publishDiscoveryRemoval((uint8_t)remote);
  char body[80];
  snprintf(body, sizeof(body), "removed remote %ld, rolling code kept\n", remote);
  _server.send(200, "text/plain", body);
}

// Streamed a line at a time: the two rings are over 10 KB together and assembling one into
// a single response would need that much again from a heap with about 30 KB free.
// Templated on the ring's depth, which is the only thing that differs between the two.
template <uint8_t Lines>
static void sendRing(ESP8266WebServer &server, const LogRing<Lines> &ring,
                     const char *whenEmpty) {
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain", ring.count() == 0 ? whenEmpty : "");
  char line[LOG_LINE_LEN + 24];
  for (uint8_t i = 0; i < ring.count(); i++) {
    ring.render(i, line, sizeof(line));
    server.sendContent(line);
    server.sendContent("\n");
  }
  server.sendContent("");
}

void WebUi::handleLog() { sendRing(_server, logBuffer(), ""); }

void WebUi::handleErrors() { sendRing(_server, errorBuffer(), "no faults recorded\n"); }

// A snapshot, computed now rather than remembered. The boot banner scrolls out of the log
// within hours; nothing here can drift out, because nothing here is stored.
void WebUi::handleStatus() {
  const uint32_t up = millis() / 1000;
  char body[512];

  snprintf(body, sizeof(body),
           "host    : %s\n"
           "build   : %s %s\n"
           "reset   : %s\n"
           "uptime  : %luh %02lum %02lus\n"
           "heap    : %u free\n"
           "wifi    : %s  ip %s  rssi %d dBm\n"
           "remotes : %u configured, %u command(s) queued\n"
           "store   : sector %c gen %lu, %u free slots, %u spent%s%s\n"
           "config  : epoch %lu by %s\n"
           "log     : %u of %u lines, %u faults\n",
           _hostname, __DATE__, __TIME__, ESP.getResetReason().c_str(),
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
           (unsigned long)(up % 60), ESP.getFreeHeap(),
           _net.connected() ? "up" : "down", WiFi.localIP().toString().c_str(),
           _net.rssi(), _remotes.count(), _remotes.pending(), _store.activeName(),
           (unsigned long)_store.generation(), _store.freeSlots(), _store.spent(),
           _store.degraded() ? "  DEGRADED" : "",
           _store.legacyHeld() ? "  legacy-held" : "",
           (unsigned long)_mqtt.config().epoch, _mqtt.config().writer,
           logBuffer().count(), LOG_LINES, errorBuffer().count());

  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "text/plain", body);

  // One line per remote, streamed for the same reason /api/state is. The rolling code is
  // here because a counter that has stopped moving is the signature of an EEPROM that is
  // no longer being written — and of shutters that will ignore the next boot's commands.
  char line[128];
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    const RemoteState &state = _remotes.state(i);
    const char *name = _mqtt.nameOf(i);
    snprintf(line, sizeof(line),
             "remote %-2u: %-7s  next code %-6lu  %lu press(es)  %s  %s\n", i,
             coverPositionName(state.position()), (unsigned long)_remotes.counter(i),
             (unsigned long)state.version(),
             _remotes.transmittable(i) ? "ready " : "BLOCKED",
             name[0] != '\0' ? name : "");
    _server.sendContent(line);
  }
  _server.sendContent("");
}
