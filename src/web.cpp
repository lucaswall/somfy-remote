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
#include "http_origin.h"
#include "somfy_frame.h"

// Names reach three hand-built JSON documents, and one of the three doors is the MQTT names
// topic, which is not ours to filter — so escape at the emitter rather than at each door.
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

// Each sendContent() on a chunked response allocates. These endpoints emit one per remote,
// per control and per name, so batching turns twenty allocations into two.
namespace {
class Chunked {
 public:
  explicit Chunked(ESP8266WebServer &server) : _server(server) {}
  ~Chunked() { flush(); }

  void add(const char *text) {
    const size_t len = strlen(text);
    if (len >= sizeof(_buf)) {
      flush();
      _server.sendContent(text);
      return;
    }
    if (_at + len >= sizeof(_buf)) {
      flush();
    }
    memcpy(_buf + _at, text, len);
    _at += len;
  }

  void flush() {
    if (_at == 0) {
      return;
    }
    _buf[_at] = '\0';
    _server.sendContent(_buf);
    _at = 0;
  }

 private:
  ESP8266WebServer &_server;
  size_t _at = 0;
  char _buf[512];
};
}   // namespace

// Long enough to walk to the control and press it, short enough that a neighbour pressing
// theirs in the meantime is unlikely.
static const uint16_t LEARN_SECONDS = 120;

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
  // Shared by every page, and the only thing here worth letting a browser cache. A day was
  // too long: a firmware update changes this stylesheet, and the phone that had already
  // cached it kept rendering the old layout with no way to know it was stale. Sixty seconds
  // still spares the refetch within a session, and a flash shows up on the next reload —
  // there is no URL version to bust, because the pages are static PROGMEM.
  _server.on("/style.css", HTTP_GET, [this]() {
    _server.sendHeader("Cache-Control", "max-age=60");
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
  _server.on("/api/control/save", HTTP_POST, [this]() { handleControlSave(); });
  _server.on("/api/control/forget", HTTP_POST, [this]() { handleControlForget(); });
  _server.on("/api/control/ignore", HTTP_POST, [this]() { handleControlIgnore(); });
  _server.on("/api/control/learn", HTTP_POST, [this]() { handleLearn(); });
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

// **The target is checked before the Origin.** Both headers come from the browser, so
// matching one against the other compares two attacker-supplied values: a page anywhere on
// the internet whose DNS answer is re-pointed at this device sends a consistent pair for
// its own name, and /api/send needs no password. Only a Host this device actually answers
// to gets past the first gate.
//
// An absent Origin is still allowed — curl and anything scripted have no reason to send
// one — but it no longer means an unchecked Host.
bool WebUi::sameOrigin() {
  const String host = _server.hostHeader();
  if (!http::hostIsOurs(host.c_str(), _hostname, WiFi.localIP().toString().c_str())) {
    _server.send(403, "text/plain", "request refused: unrecognised Host\n");
    logError("web       : refused Host '%s'", host.c_str());
    return false;
  }

  if (!_server.hasHeader("Origin")) {
    return true;
  }
  if (http::originMatchesHost(_server.header("Origin").c_str(), host.c_str())) {
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
  Chunked out(_server);

  snprintf(chunk, sizeof(chunk),
           "{\"listening\":%s,\"learning\":%s,\"learnLeft\":%lu,"
           "\"edges\":%lu,\"frames\":%lu,\"presses\":%lu,"
           "\"mutes\":%u,\"muted\":%s,\"overflows\":%lu,\"abandoned\":%lu,"
           "\"badsum\":%lu,\"ignored\":%u,\"heard\":[",
           _receiver.listening() ? "true" : "false",
           _receiver.discovering() ? "true" : "false",
           (unsigned long)_receiver.discoverSecondsLeft(),
           (unsigned long)_receiver.edgesPerSecond(), (unsigned long)rx.frames,
           (unsigned long)rx.presses, rx.mutes, rx.muted ? "true" : "false",
           (unsigned long)rx.overflows, (unsigned long)rx.abandoned,
           (unsigned long)rx.badChecksum, rx.ignored);
  out.add(chunk);

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
    out.add(chunk);
    first = false;
  }

  out.add("],\"known\":[");
  for (uint8_t i = 0; i < controls.count(); i++) {
    const ctl::Control &control = controls.at(i);
    snprintf(chunk, sizeof(chunk), "%s{\"a\":%lu,\"name\":\"", i == 0 ? "" : ",",
             (unsigned long)control.address);
    appendJsonString(chunk, sizeof(chunk), control.name);
    out.add(chunk);
    snprintf(chunk, sizeof(chunk), "\",\"d\":%lu,\"last\":%ld}",
             (unsigned long)control.drives,
             control.lastMs == 0 ? -1L : (long)(now - control.lastMs));
    out.add(chunk);
  }

  // Display only, as everywhere else: every internal path is the index.
  out.add("],\"names\":[");
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    snprintf(chunk, sizeof(chunk), "%s\"", i == 0 ? "" : ",");
    appendJsonString(chunk, sizeof(chunk), _mqtt.nameOf(i));
    out.add(chunk);
    out.add("\"");
  }
  out.add("]}");
  out.flush();
  _server.sendContent("");
}

// Opens the window in which one unrecognised address may join the list.
void WebUi::handleLearn() {
  if (!sameOrigin() || !settingsAuthorised()) {
    return;
  }
  _receiver.discover(LEARN_SECONDS);
  _server.send(200, "text/plain", "learning\n");
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

  // Scanned and refused, never filtered. Silently deleting characters out of a typed name
  // is the same class of surprise as silently altering a typed address, and the length was
  // being measured after the strip — so a 41-character name with two quotes used to pass.
  const String requested = _server.arg("name");
  if (requested.length() == 0) {
    _server.send(400, "text/plain", "a name is required\n");
    return;
  }
  if (!ctl::nameIsAcceptable(requested.c_str())) {
    _server.send(400, "text/plain",
                 "a name cannot contain a quote, a backslash or a control character\n");
    return;
  }
  // Refused rather than cut: a truncated name still looks like a name. Measured on the raw
  // bytes, which is what the device stores — the page's maxlength counts UTF-16 code units,
  // so for accented names the two limits disagree by design.
  if (requested.length() >= ctl::NAME_LEN) {
    char message[64];
    snprintf(message, sizeof(message), "name is longer than %u characters\n",
             (unsigned)(ctl::NAME_LEN - 1));
    _server.send(400, "text/plain", message);
    return;
  }
  strncpy(control.name, requested.c_str(), ctl::NAME_LEN - 1);
  control.name[ctl::NAME_LEN - 1] = '\0';

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

  const HaMqtt::SaveResult saved = _mqtt.saveControl(control);
  if (saved == HaMqtt::SaveResult::NoRoom) {
    char message[80];
    snprintf(message, sizeof(message), "no room for another control — %u is the limit\n",
             (unsigned)ctl::MAX_CONTROLS);
    _server.send(409, "text/plain", message);
    return;
  }
  if (saved == HaMqtt::SaveResult::NotPublished) {
    // In effect now, not durable yet. The retained topic is the only copy that survives a
    // restart, so say which half worked rather than sending somebody back to re-walk it.
    _receiver.forgetSighting(control.address);
    _server.send(503, "text/plain",
                 "saved and working now, but the broker did not take it — it will not "
                 "survive a restart until it does\n");
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
  Chunked out(_server);
  const uint16_t count = _receiver.captureCount();
  snprintf(line, sizeof(line), "# %u intervals, %s\n", count,
           _receiver.captureFrozen() ? "frozen on a failed frame" : "still running");
  out.add(line);
  for (uint16_t i = 0; i < count; i++) {
    const uint16_t entry = _receiver.captureAt(i);
    snprintf(line, sizeof(line), "%c %u\n", (entry & 0x8000u) ? 'H' : 'L', entry & 0x7FFFu);
    out.add(line);
  }
  out.flush();
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
  Chunked out(_server);

  snprintf(chunk, sizeof(chunk),
           "{\"host\":\"%s\",\"ip\":\"%s\",\"rssi\":%d,\"uptime\":%lu,\"heap\":%u,"
           "\"pending\":%u,\"configured\":%s,\"degraded\":%s,"
           "\"store\":\"%c\",\"free\":%u,\"epoch\":%lu,\"remotes\":[",
           _hostname, WiFi.localIP().toString().c_str(), _net.rssi(), (unsigned long)up,
           ESP.getFreeHeap(), _remotes.pending(), _mqtt.configured() ? "true" : "false",
           _store.degraded() ? "true" : "false",
           _store.activeName(), _store.freeSlots(),
           (unsigned long)_mqtt.config().epoch);
  out.add(chunk);

  // The name comes from Home Assistant and is shown, never used to key anything: every
  // internal path here is the index. A remote with no name published simply reads as its
  // index, which is what the page did before names existed.
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    const RemoteState &state = _remotes.state(i);
    snprintf(chunk, sizeof(chunk), "%s{\"n\":%u,\"name\":\"", i == 0 ? "" : ",", i);
    appendJsonString(chunk, sizeof(chunk), _mqtt.nameOf(i));
    out.add(chunk);
    snprintf(chunk, sizeof(chunk),
             "\",\"position\":\"%s\",\"pct\":%d,\"travel\":%u,\"code\":%lu,"
             "\"version\":%lu,\"enabled\":%s,\"operational\":%s,\"ready\":%s}",
             coverPositionName(state.position()), state.percent(millis()),
             (unsigned)(state.travelMs() / 1000), (unsigned long)_remotes.counter(i),
             (unsigned long)state.version(), _remotes.enabled(i) ? "true" : "false",
             _remotes.operational(i) ? "true" : "false",
             _remotes.transmittable(i) ? "true" : "false");
    out.add(chunk);
  }

  out.add("]}");
  out.flush();
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
                 : _remotes.adoptFailed((uint8_t)remote)
                     ? "remote's adopted rolling code could not be stored\n"
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
  if (_server.hasArg("travel")) {
    const String seconds = _server.arg("travel");
    char *tail = nullptr;
    const long value = strtol(seconds.c_str(), &tail, 10);
    if (seconds.length() == 0 || *tail != '\0' || value < 1 ||
        value > cfg::MAX_TRAVEL_SECONDS) {
      char why[72];
      snprintf(why, sizeof(why), "travel time must be 1 to %u seconds\n",
               cfg::MAX_TRAVEL_SECONDS);
      _server.send(400, "text/plain", why);
      return;
    }
    entry->travelSeconds = (uint8_t)value;
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

  char body[112];
  snprintf(body, sizeof(body), "remote %ld: %s, %s, %us travel\n", remote,
           entry->enabled ? "in Home Assistant" : "removed",
           entry->operational ? "operational" : "not operational",
           entry->travelSeconds > 0 ? entry->travelSeconds
                                    : (unsigned)(COVER_TRAVEL_MS / 1000));
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
                 : _remotes.adoptFailed((uint8_t)remote)
                     ? "remote's adopted rolling code could not be stored\n"
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

  // Absent means "derive it from the base". Present and unparseable is refused rather than
  // collapsed into the derived address — a typed address that quietly becomes a different
  // one is how a remote ends up driving somebody else's motor.
  const String addr = _server.arg("address");
  uint32_t address = rs::ADDR_NONE;
  if (addr.length() > 0 && !cfg::parseAddress(addr.c_str(), &address)) {
    _server.send(400, "text/plain",
                 "address must be 1-6 hex digits, optionally 0x-prefixed (24 bits)\n");
    return;
  }
  next.remotes[next.entries].index = index;
  next.remotes[next.entries].address = address;
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
  Chunked out(_server);

  // One line per remote, streamed for the same reason /api/state is. The rolling code is
  // here because a counter that has stopped moving while presses are logged is the
  // signature of a flash that has stopped accepting writes — and of shutters that will
  // ignore the next boot's commands.
  // 192, not 128: the counters line is nine numbers and a suffix, which at full width is
  // about 185 characters — gcc's -Wformat-truncation was right, and a truncated diagnostic
  // is worst exactly when the numbers are large enough to matter.
  char line[192];
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    const RemoteState &state = _remotes.state(i);
    const char *name = _mqtt.nameOf(i);
    snprintf(line, sizeof(line),
             "remote %-2u: %-7s  next code %-6lu  %lu press(es)  %s  %s\n", i,
             coverPositionName(state.position()), (unsigned long)_remotes.counter(i),
             (unsigned long)state.version(),
             _remotes.transmittable(i) ? "ready " : "BLOCKED",
             name[0] != '\0' ? name : "");
    out.add(line);
  }

  // Always, not only when armed: a muted receiver and a quiet house look identical.
  const Receiver::Stats rx = _receiver.stats();
  snprintf(line, sizeof(line),
           "receiver: %s  marcstate 0x%02X%s  %lu edges/s  rssi %d/%d dBm  "
           "%lu frames  %lu presses  %u known\n",
           _receiver.listening() ? "listening" : "MUTED", rx.marcState,
           rx.marcState == CC1101_STATE_RX ? "" : " NOT RX",
           (unsigned long)_receiver.edgesPerSecond(), rx.rssiNow, rx.rssiPeak,
           (unsigned long)rx.frames,
           (unsigned long)rx.presses, _mqtt.controls().count());
  out.add(line);
  snprintf(line, sizeof(line),
           "        : %lu int, %lu ring, %lu overflow, %lu abandoned, "
           "%lu bad checksum, %u mutes, peak %u/10ms, %lu level repeats%s\n",
           (unsigned long)rx.interrupts,
           (unsigned long)rx.ringWrites, (unsigned long)rx.overflows,
           (unsigned long)rx.abandoned, (unsigned long)rx.badChecksum, rx.mutes,
           (unsigned)rx.peakRate, (unsigned long)rx.levelRepeats,
           rx.ownAddress > 0    ? "  OWN ADDRESS HEARD"
           : rx.pressesDropped > 0 ? "  PRESSES DROPPED"
                                   : "");
  out.add(line);
  out.flush();
  _server.sendContent("");
}
