#include "web.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <stdlib.h>

#include "build_info.h"
#include "config_doc.h"
#include "log.h"
#include "page.h"
#include "secrets.h"
#include "control_map.h"
#include "somfy_frame.h"

// Names come from Home Assistant and from the control form, into JSON built with snprintf.
// One double quote makes /api/state unparseable, and the operation page's poll() only marks
// itself stale — so it shows no shutters and no message, permanently, with /status fine and
// nothing pointing at the cause. Escaped at the emitter because one of the three doors is
// the MQTT names topic, which is not ours to filter.
static void appendJsonString(char *out, size_t cap, const char *in) {
  size_t at = strlen(out);
  for (; *in != '\0' && at + 7 < cap; in++) {
    const unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') {
      out[at++] = '\\';
      out[at++] = (char)c;
    } else if (c < 0x20) {
      at += (size_t)snprintf(out + at, cap - at, "\\u%04x", c);
      continue;
    } else {
      out[at++] = (char)c;
    }
  }
  out[at] = '\0';
}

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
  _server.on("/api/remote/flags", HTTP_POST, [this]() { handleRemoteFlags(); });
  _server.on("/controls", HTTP_GET, [this]() { handleControls(); });
  _server.on("/api/heard", HTTP_GET, [this]() { handleHeard(); });
  _server.on("/api/receiver/arm", HTTP_POST, [this]() { handleArm(); });
  _server.on("/api/control/save", HTTP_POST, [this]() { handleControlSave(); });
  _server.on("/api/control/forget", HTTP_POST, [this]() { handleControlForget(); });
  _server.on("/api/control/ignore", HTTP_POST, [this]() { handleControlIgnore(); });
  _server.on("/api/capture", HTTP_GET, [this]() { handleCapture(); });
  _server.onNotFound([this]() { _server.send(404, "text/plain", "not found"); });

  _server.collectHeaders("Origin");   // the server discards headers it was not told to keep
  _server.begin();

  // ArduinoOTA already started mDNS under the same hostname; advertising the web service
  // is what makes the board show up as a browsable device rather than just a pingable one.
  MDNS.addService("http", "tcp", 80);

  _started = true;
  logLine("web       : http://%s.local/  (http://%s/)", _hostname,
          WiFi.localIP().toString().c_str());
}

// "Anyone in the house" and "any website anyone in the house visits" are different sets,
// and neither a cross-origin form post nor a no-cors fetch triggers a preflight.
//
// An absent Origin is allowed — curl and anything scripted have no reason to send one. A
// present one is compared against the Host header rather than a fixed name, because the UI
// is reached both by IP and as <hostname>.local.
bool WebUi::sameOrigin() {
  if (!_server.hasHeader("Origin")) {
    return true;
  }
  const String origin = _server.header("Origin");
  const int slashes = origin.indexOf("//");
  const String authority = slashes < 0 ? origin : origin.substring(slashes + 2);
  if (authority == _server.hostHeader()) {
    return true;
  }
  _server.send(403, "text/plain", "cross-origin request refused\n");
  logError("web       : cross-origin POST refused");
  return false;
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

void WebUi::handleControls() {
  if (!settingsAuthorised()) {
    return;
  }
  _server.sendHeader("Cache-Control", "no-store");
  _server.send_P(200, "text/html", CONTROLS_HTML);
}

// **Every foreign RF address leaves through here and nowhere else.** Not folded into
// /api/state, which the open pages poll: an address is the credential of a motor.
void WebUi::handleHeard() {
  if (!settingsAuthorised()) {
    return;
  }

  const Receiver::Stats rx = _receiver.stats();
  const uint32_t now = millis();
  char chunk[256];

  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "application/json", "");

  snprintf(chunk, sizeof(chunk),
           "{\"armed\":%s,\"left\":%lu,\"edges\":%lu,\"frames\":%lu,\"presses\":%lu,"
           "\"mutes\":%u,\"muted\":%s,\"overflows\":%lu,\"abandoned\":%lu,"
           "\"badsum\":%lu,\"heard\":[",
           _receiver.armed() ? "true" : "false", (unsigned long)_receiver.secondsLeft(),
           (unsigned long)_receiver.edgesPerSecond(), (unsigned long)rx.frames,
           (unsigned long)rx.presses, rx.mutes, rx.muted ? "true" : "false",
           (unsigned long)rx.overflows, (unsigned long)rx.abandoned,
           (unsigned long)rx.badChecksum);
  _server.sendContent(chunk);

  const ctl::ControlMap &controls = _mqtt.controls();
  bool first = true;
  for (uint8_t i = 0; i < _receiver.sightingCount(); i++) {
    const Sighting &sighting = _receiver.sighting(i);
    if (controls.find(sighting.address) != nullptr) {
      continue;   // named already: it belongs in the other list
    }
    snprintf(chunk, sizeof(chunk),
             "%s{\"a\":%lu,\"n\":%u,\"code\":%u,\"cmd\":%u,\"last\":%lu,\"first\":%lu}",
             first ? "" : ",", (unsigned long)sighting.address, sighting.presses,
             sighting.lastCode, sighting.lastCommand,
             (unsigned long)(now - sighting.lastMs), (unsigned long)(now - sighting.firstMs));
    _server.sendContent(chunk);
    first = false;
  }

  _server.sendContent("],\"known\":[");
  for (uint8_t i = 0; i < controls.count(); i++) {
    const ctl::Control &control = controls.at(i);
    snprintf(chunk, sizeof(chunk), "%s{\"a\":%lu,\"name\":\"", i == 0 ? "" : ",",
             (unsigned long)control.address);
    appendJsonString(chunk, sizeof(chunk), control.name);
    _server.sendContent(chunk);
    snprintf(chunk, sizeof(chunk), "\",\"d\":%lu}", (unsigned long)control.drives);
    _server.sendContent(chunk);
  }

  // Display only, as everywhere else: every internal path is the index.
  _server.sendContent("],\"names\":[");
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    snprintf(chunk, sizeof(chunk), "%s\"", i == 0 ? "" : ",");
    appendJsonString(chunk, sizeof(chunk), _mqtt.nameOf(i));
    _server.sendContent(chunk);
    _server.sendContent("\"");
  }
  _server.sendContent("]}");
  _server.sendContent("");
}

// Zero stops; anything else starts or extends without restarting the radio mid-frame.
void WebUi::handleArm() {
  if (!sameOrigin() || !settingsAuthorised()) {
    return;
  }
  const long minutes = _server.arg("minutes").toInt();
  if (minutes <= 0) {
    _receiver.disarm();
    _server.send(200, "text/plain", "stopped\n");
    return;
  }
  if (minutes > 240) {
    _server.send(400, "text/plain", "at most 240 minutes\n");
    return;
  }
  if (!_receiver.arm((uint16_t)minutes)) {
    _server.send(503, "text/plain", "the radio is not ready\n");
    return;
  }
  _server.send(200, "text/plain", "listening\n");
}

void WebUi::handleControlSave() {
  if (!sameOrigin() || !settingsAuthorised()) {
    return;
  }
  if (!_server.hasArg("address") || !_server.hasArg("name")) {
    _server.send(400, "text/plain", "address and name are required\n");
    return;
  }

  ctl::Control control = {};
  control.address = (uint32_t)strtoul(_server.arg("address").c_str(), nullptr, 10) & 0xFFFFFFu;

  const String requested = _server.arg("name");
  uint8_t kept = 0;
  for (uint16_t i = 0; i < requested.length() && kept < ctl::NAME_LEN - 1; i++) {
    const char c = requested[i];
    if (c == '"' || c == '\\' || (uint8_t)c < 0x20) {
      continue;
    }
    control.name[kept++] = c;
  }
  control.name[kept] = '\0';
  if (kept == 0) {
    _server.send(400, "text/plain", "a name is required\n");
    return;
  }

  // "0,2,3" — the indices this control drives. Only the static bound is applied here;
  // whether an index currently exists is decided when a press arrives, because the
  // configuration is a separately versioned document that may not have landed yet.
  const String drives = _server.arg("drives");
  int at = 0;
  while (at < (int)drives.length()) {
    const int comma = drives.indexOf(',', at);
    const int end = comma < 0 ? drives.length() : comma;
    // Not toInt(): it reads anything non-numeric as 0, so one stray separator would make the
    // control claim it drives remote 0 — retained, and surviving replacement boards.
    const String piece = drives.substring(at, end);
    char *stop = nullptr;
    const long index = strtol(piece.c_str(), &stop, 10);
    if (stop != piece.c_str() && *stop == '\0' && index >= 0 && index < rs::MAX_REMOTES) {
      control.drives |= (uint32_t)1u << index;
    }
    at = end + 1;
  }

  if (!_mqtt.saveControl(control)) {
    // The retained topic is the only durable copy: reporting success for a save the broker
    // never took would lose an hour of walking at the next restart.
    _server.send(503, "text/plain", "the broker did not accept it — nothing was saved\n");
    return;
  }
  _receiver.forgetSighting(control.address);
  _server.send(200, "text/plain", "saved\n");
}

void WebUi::handleControlForget() {
  if (!sameOrigin() || !settingsAuthorised()) {
    return;
  }
  const uint32_t address =
      (uint32_t)strtoul(_server.arg("address").c_str(), nullptr, 10) & 0xFFFFFFu;
  if (!_mqtt.forgetControl(address)) {
    _server.send(503, "text/plain", "the broker did not accept it\n");
    return;
  }
  _server.send(200, "text/plain", "forgotten\n");
}

// It comes back if heard again: this is a work list, not a block list.
void WebUi::handleControlIgnore() {
  if (!sameOrigin() || !settingsAuthorised()) {
    return;
  }
  _receiver.forgetSighting(
      (uint32_t)strtoul(_server.arg("address").c_str(), nullptr, 10) & 0xFFFFFFu);
  _server.send(200, "text/plain", "dropped\n");
}

// The tool for the one question the counters cannot answer: a decoder rejecting everything
// and a radio hearing nothing look identical from outside.
void WebUi::handleCapture() {
  if (!settingsAuthorised()) {
    return;
  }

  char line[48];
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "text/plain", "");
  const uint16_t count = _receiver.captureCount();
  snprintf(line, sizeof(line), "# %u intervals, %s\n", count,
           _receiver.captureFrozen() ? "frozen on a failed frame" : "still running");
  _server.sendContent(line);
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t entry = _receiver.captureAt(i);
    snprintf(line, sizeof(line), "%c %u\n", (entry & 0x8000u) ? 'H' : 'L', entry & 0x7FFFu);
    _server.sendContent(line);
  }
  _server.sendContent("");

  // Opt-in: a frozen capture is often the only artefact of a failure somebody had to press a
  // remote to produce, and a reload or a prefetch would wipe it.
  if (_server.hasArg("rearm")) {
    _receiver.rearmCapture();
  }
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
           "\"pending\":%u,\"configured\":%s,\"degraded\":%s,"
           "\"store\":\"%c\",\"free\":%u,\"epoch\":%lu,\"remotes\":[",
           _hostname, WiFi.localIP().toString().c_str(), _net.rssi(), (unsigned long)up,
           ESP.getFreeHeap(), _remotes.pending(), _mqtt.configured() ? "true" : "false",
           _store.degraded() ? "true" : "false",
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
  // The one mutating endpoint with no password, so the only thing standing between a
  // shutter and a page somebody in the house happened to open.
  if (!sameOrigin()) {
    return;
  }
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

// Sets either flag on one remote. Both are absent-means-unchanged, so the page can toggle
// one without having to restate the other.
//
// `operational` is the guard the send path consults: false and the firmware refuses to
// transmit for that remote, whoever asks and by whatever route. It is for a shutter that
// is known not to work, so that "do not drive this one" is enforced rather than remembered.
//
// `enabled` is whether Home Assistant has entities for it at all. Turning it back on is how
// a removed remote comes back — it keeps its index and its rolling code, so it resumes
// where it left off rather than restarting a counter a motor has already seen.
void WebUi::handleRemoteFlags() {
  if (!sameOrigin() || !settingsAuthorised()) {
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
  cfg::RemoteConfig *entry = nullptr;
  for (uint8_t i = 0; i < next.entries; i++) {
    if (next.remotes[i].index == (uint8_t)remote) {
      entry = &next.remotes[i];
    }
  }
  if (entry == nullptr) {
    _server.send(404, "text/plain", "no such remote\n");
    return;
  }

  const bool wasEnabled = entry->enabled;
  if (_server.hasArg("operational")) {
    entry->operational = _server.arg("operational") == "1";
  }
  if (_server.hasArg("enabled")) {
    entry->enabled = _server.arg("enabled") == "1";
  }

  if (!_mqtt.applyConfig(next, "ui")) {
    _server.send(500, "text/plain", "could not persist configuration\n");
    return;
  }
  // Going the other way needs the entities taken out of Home Assistant explicitly; a
  // discovery config that is simply no longer republished stays where it is.
  if (wasEnabled && !entry->enabled) {
    _mqtt.publishDiscoveryRemoval((uint8_t)remote);
  }

  char body[96];
  snprintf(body, sizeof(body), "remote %ld: %s, %s\n", remote,
           entry->enabled ? "in Home Assistant" : "removed",
           entry->operational ? "operational" : "not operational");
  _server.send(200, "text/plain", body);
}

// Behind the settings password. Held down at the motor, Prog enrols or drops this emulated
// remote — the one press here that changes something permanent rather than something that
// can be pressed back.
void WebUi::handleProg() {
  if (!sameOrigin() || !settingsAuthorised()) {
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
  if (!sameOrigin() || !settingsAuthorised()) {
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
  if (!sameOrigin() || !settingsAuthorised()) {
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
           "build   : %s\n"
           "reset   : %s\n"
           "uptime  : %luh %02lum %02lus\n"
           "heap    : %u free\n"
           "wifi    : %s  ip %s  rssi %d dBm\n"
           "remotes : %u configured, %u command(s) queued\n"
           "store   : sector %c gen %lu, %u free slots, %u spent%s\n"
           "config  : epoch %lu by %s\n"
           "log     : %u of %u lines, %u faults\n",
           _hostname, BUILD_STAMP, ESP.getResetReason().c_str(),
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
           (unsigned long)(up % 60), ESP.getFreeHeap(),
           _net.connected() ? "up" : "down", WiFi.localIP().toString().c_str(),
           _net.rssi(), _remotes.count(), _remotes.pending(), _store.activeName(),
           (unsigned long)_store.generation(), _store.freeSlots(), _store.spent(),
           _store.degraded() ? "  DEGRADED" : "",
           (unsigned long)_mqtt.config().epoch, _mqtt.config().writer,
           logBuffer().count(), LOG_LINES, errorBuffer().count());

  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "text/plain", body);

  // One line per remote, streamed for the same reason /api/state is. The rolling code is
  // here because a counter that has stopped moving while presses are logged is the
  // signature of a flash that has stopped accepting writes — and of shutters that will
  // ignore the next boot's commands.
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

  // Always, not only when armed: a muted receiver and a quiet house look identical.
  const Receiver::Stats rx = _receiver.stats();
  snprintf(line, sizeof(line),
           "receiver: %s  %lu edges/s  %lu frames  %lu presses  %u known\n",
           _receiver.armed() ? (rx.muted ? "MUTED" : "listening") : "off",
           (unsigned long)_receiver.edgesPerSecond(), (unsigned long)rx.frames,
           (unsigned long)rx.presses, _mqtt.controls().count());
  _server.sendContent(line);
  snprintf(line, sizeof(line),
           "        : %lus left, %lu int, %lu ring, %lu overflow, %lu abandoned, "
           "%lu bad checksum, %u mutes, peak %u/10ms, %lu level repeats%s\n",
           (unsigned long)_receiver.secondsLeft(), (unsigned long)rx.interrupts,
           (unsigned long)rx.ringWrites, (unsigned long)rx.overflows,
           (unsigned long)rx.abandoned, (unsigned long)rx.badChecksum, rx.mutes,
           (unsigned)rx.peakRate, (unsigned long)rx.levelRepeats,
           rx.ownAddress > 0    ? "  OWN ADDRESS HEARD"
           : rx.pressesDropped > 0 ? "  PRESSES DROPPED"
                                   : "");
  _server.sendContent(line);
  _server.sendContent("");
}
