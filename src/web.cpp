#include "web.h"

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>

#include "log.h"
#include "page.h"
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
  _server.on("/api/state", HTTP_GET, [this]() { handleState(); });
  _server.on("/api/send", HTTP_POST, [this]() { handleSend(); });
  _server.on("/log", HTTP_GET, [this]() { handleLog(); });
  _server.on("/errors", HTTP_GET, [this]() { handleErrors(); });
  _server.on("/status", HTTP_GET, [this]() { handleStatus(); });
  _server.onNotFound([this]() { _server.send(404, "text/plain", "not found"); });
  _server.begin();

  // ArduinoOTA already started mDNS under the same hostname; advertising the web service
  // is what makes the board show up as a browsable device rather than just a pingable one.
  MDNS.addService("http", "tcp", 80);

  _started = true;
  logLine("web       : http://%s.local/  (http://%s/)", _hostname,
          WiFi.localIP().toString().c_str());
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
           "\"pending\":%u,\"remotes\":[",
           _hostname, WiFi.localIP().toString().c_str(), _net.rssi(), (unsigned long)up,
           ESP.getFreeHeap(), _remotes.pending());
  _server.sendContent(chunk);

  for (uint8_t i = 0; i < _remotes.count(); i++) {
    const RemoteState &state = _remotes.state(i);
    snprintf(chunk, sizeof(chunk),
             "%s{\"n\":%u,\"position\":\"%s\",\"code\":%u,\"version\":%lu}",
             i == 0 ? "" : ",", i, coverPositionName(state.position()),
             _remotes.rollingCode(i), (unsigned long)state.version());
    _server.sendContent(chunk);
  }

  _server.sendContent("]}");
  _server.sendContent("");
}

void WebUi::handleSend() {
  const long remote = _server.arg("remote").toInt();
  const String button = _server.arg("command");

  SomfyCommand command;
  if (!somfyCommandFromText(button.c_str(), button.length(), &command)) {
    _server.send(400, "text/plain", "unknown command\n");
    return;
  }
  if (remote < 0 || remote >= _remotes.count()) {
    _server.send(404, "text/plain", "no such remote\n");
    return;
  }

  _remotes.queue((uint8_t)remote, command);
  handleState();
}

// Streamed a line at a time: the two rings are over 10 KB together and assembling one into
// a single response would need that much again from a heap with about 30 KB free.
void WebUi::handleLog() {
  const LogBuffer &log = logBuffer();
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "text/plain", "");
  char line[LOG_LINE_LEN + 24];
  for (uint8_t i = 0; i < log.count(); i++) {
    log.render(i, line, sizeof(line));
    _server.sendContent(line);
    _server.sendContent("\n");
  }
  _server.sendContent("");
}

void WebUi::handleErrors() {
  const ErrorBuffer &errors = errorBuffer();
  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "text/plain", errors.count() == 0 ? "no faults recorded\n" : "");
  char line[LOG_LINE_LEN + 24];
  for (uint8_t i = 0; i < errors.count(); i++) {
    errors.render(i, line, sizeof(line));
    _server.sendContent(line);
    _server.sendContent("\n");
  }
  _server.sendContent("");
}

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
           "remotes : %u emulated, %u command(s) queued\n"
           "log     : %u of %u lines, %u faults\n",
           _hostname, __DATE__, __TIME__, ESP.getResetReason().c_str(),
           (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
           (unsigned long)(up % 60), ESP.getFreeHeap(),
           _net.connected() ? "up" : "down", WiFi.localIP().toString().c_str(),
           _net.rssi(), _remotes.count(), _remotes.pending(), logBuffer().count(),
           LOG_LINES, errorBuffer().count());

  _server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  _server.sendHeader("Cache-Control", "no-store");
  _server.send(200, "text/plain", body);

  // One line per remote, streamed for the same reason /api/state is. The rolling code is
  // here because a counter that has stopped moving is the signature of an EEPROM that is
  // no longer being written — and of shutters that will ignore the next boot's commands.
  char line[80];
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    const RemoteState &state = _remotes.state(i);
    snprintf(line, sizeof(line), "remote %-2u: %-7s  next code %-5u  %lu press(es)\n", i,
             coverPositionName(state.position()), _remotes.rollingCode(i),
             (unsigned long)state.version());
    _server.sendContent(line);
  }
  _server.sendContent("");
}
