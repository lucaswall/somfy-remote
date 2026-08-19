#include "ha_mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>

#include "log.h"
#include "secrets.h"
#include "timing.h"
#include "topics.h"

// Backed off to a cap: a broker that is simply switched off should not have us blocking on
// a connect attempt every few seconds. Each attempt costs the socket timeout below, and
// while it blocks, ArduinoOTA and the radio do not run.
static const uint32_t RETRY_MIN_MS = 5000;
static const uint32_t RETRY_MAX_MS = 60000;

// PubSubClient defaults to 15 s and WiFiClient to 5 s. Fifteen seconds without servicing
// ArduinoOTA is longer than espota waits for an invitation, which on a board in a case
// means a broker outage could cost the ability to reflash. Two seconds is ample on a LAN.
static const uint16_t SOCKET_TIMEOUT_S = 2;
static const uint32_t CLIENT_TIMEOUT_MS = 2000;

// The largest discovery payload is the switch's, at just over 500 bytes with a device id
// of a realistic length. The margin is for a longer one.
static const size_t PAYLOAD_LEN = 768;

// snprintf truncates silently, and two truncated topics are one topic: remote 2 and
// remote 21 would share a command topic and move together. The longest this firmware
// builds is "<id>/remote29/my_state", so fail the build rather than the installation.
static_assert(sizeof(MQTT_DEVICE_ID) + sizeof("/remote29/my_state") - 1 <= TOPIC_LEN,
              "MQTT_DEVICE_ID is too long: see TOPIC_LEN in include/topics.h");

void HaMqtt::loop() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!_mqtt.connected()) {
    const uint32_t now = millis();
    if (_attempted && !elapsed(now, _lastAttempt, _retryMs)) {
      return;
    }
    _attempted = true;
    const bool ok = connect();
    // Stamped after the attempt, not before: a connect that burns its whole timeout would
    // otherwise return with the retry window already expired and spin.
    _lastAttempt = millis();
    if (!ok) {
      _retryMs = _retryMs * 2 >= RETRY_MAX_MS ? RETRY_MAX_MS : _retryMs * 2;
      return;
    }
    _retryMs = RETRY_MIN_MS;
  }

  _mqtt.loop();

  // Publish on any change, whoever caused it — a command from Home Assistant or a press
  // on the web page.
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    if (_remotes.state(i).version() != _publishedVersion[i]) {
      _publishedVersion[i] = _remotes.state(i).version();
      publishState(i);
    }
  }
}

bool HaMqtt::connect() {
  char availability[TOPIC_LEN];
  topicAvailability(availability, sizeof(availability), MQTT_DEVICE_ID);

  _mqtt.setServer(MQTT_HOST, MQTT_PORT);
  _mqtt.setBufferSize(1024);   // the discovery payloads do not fit the 256-byte default
  _mqtt.setSocketTimeout(SOCKET_TIMEOUT_S);
  _wifi.setTimeout(CLIENT_TIMEOUT_MS);
  _mqtt.setCallback([this](char *topic, uint8_t *payload, unsigned int length) {
    onMessage(topic, payload, length);
  });

  // The last will is the whole availability story: a crashed bridge looks exactly like an
  // idle one, so the broker has to be the one to say we are gone. The firmware this
  // replaces had none, which is why a dead bridge showed up as shutters that simply
  // stopped responding.
  if (!_mqtt.connect(_clientId, MQTT_USER, MQTT_PASSWORD, availability, 0, true,
                     "offline")) {
    logError("mqtt      : connect failed, state %d", _mqtt.state());
    return false;
  }

  _mqtt.publish(availability, "online", true);

  char wildcard[TOPIC_LEN];
  topicCommandWildcard(wildcard, sizeof(wildcard), MQTT_DEVICE_ID);
  if (!_mqtt.subscribe(wildcard)) {
    logError("mqtt      : subscribe to %s rejected", wildcard);
  }

  // Retained, so Home Assistant recreates the entities after its own restart without
  // waiting for us to reconnect — and republished on every reconnect, because a broker
  // that lost its retained set is exactly what a reconnect looks like from here. State
  // goes with it for the same reason: republishing the config alone brings the entities
  // back blank, which is the failure the retained state topic exists to prevent.
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    publishDiscovery(i);
    publishState(i);
  }

  logLine("mqtt      : connected to %s as %s, %u remotes announced", MQTT_HOST, MQTT_USER,
          _remotes.count());
  return true;
}

void HaMqtt::publishDiscovery(uint8_t remote) {
  char availability[TOPIC_LEN], command[TOPIC_LEN], state[TOPIC_LEN], myState[TOPIC_LEN];
  topicAvailability(availability, sizeof(availability), MQTT_DEVICE_ID);
  topicCommand(command, sizeof(command), MQTT_DEVICE_ID, remote);
  topicCoverState(state, sizeof(state), MQTT_DEVICE_ID, remote);
  topicMyState(myState, sizeof(myState), MQTT_DEVICE_ID, remote);

  char identifier[OBJECT_ID_LEN], name[DEVICE_NAME_LEN], url[48];
  deviceIdentifier(identifier, sizeof(identifier), MQTT_DEVICE_ID, remote);
  deviceName(name, sizeof(name), remote);
  snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());

  char object[OBJECT_ID_LEN], topic[DISCOVERY_TOPIC_LEN];
  char payload[PAYLOAD_LEN];

  // Every string handed to ArduinoJson below is a char array, which it copies. Passing a
  // const char* instead would store the pointer, and these all go out of scope here.
  for (uint8_t entity = 0; entity < 3; entity++) {
    JsonDocument doc;
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = identifier;
    device["name"] = name;
    device["model"] = "Wemos Somfy Remote";
    device["manufacturer"] = "MLCM Tech";
    device["sw_version"] = __DATE__;
    device["configuration_url"] = url;

    doc["availability_topic"] = availability;
    doc["command_topic"] = command;

    const char *component = nullptr;
    switch (entity) {
      case 0:
        // Prog. Held down, it enrolls this emulated remote with a motor, which is the one
        // action here that changes something physical and permanent.
        component = "button";
        uniqueId(object, sizeof(object), MQTT_DEVICE_ID, remote, "prog");
        doc["name"] = "Prog";
        doc["payload_press"] = "Prog";
        break;
      case 1:
        // My as a switch rather than a button, because Google Home does not expose
        // buttons usefully. Both payloads are the same press; state_on and state_off are
        // what make it fall back to off afterwards instead of sitting in Home Assistant
        // as permanently unknown, which is what the firmware this replaces did.
        component = "switch";
        uniqueId(object, sizeof(object), MQTT_DEVICE_ID, remote, "my");
        doc["name"] = "My";
        doc["state_topic"] = myState;
        doc["payload_on"] = "My";
        doc["payload_off"] = "My";
        doc["state_on"] = "on";
        doc["state_off"] = "off";
        break;
      default:
        // The cover, and the primary entity of the device: a null name means it inherits
        // the device's, which is what makes it cover.somfy_remote<n> rather than
        // cover.somfy_remote<n>_cover.
        component = "cover";
        uniqueId(object, sizeof(object), MQTT_DEVICE_ID, remote, "cover");
        doc["name"] = nullptr;
        doc["state_topic"] = state;
        doc["payload_open"] = "Up";
        doc["payload_close"] = "Down";
        doc["payload_stop"] = "My";
        // RTS is one-way. Home Assistant shows both buttons at all times rather than
        // hiding the one it thinks is redundant, because what it thinks may be wrong.
        // The key is `optimistic`: MQTT discovery drops anything outside its schema, and
        // `assumed_state` — which is what the attribute is called on the entity — is not
        // in it, so asking for it by that name asks for nothing.
        doc["optimistic"] = true;
        break;
    }

    doc["unique_id"] = object;
    discoveryTopic(topic, sizeof(topic), HA_DISCOVERY_PREFIX, component, object);

    // Truncation is checked separately from the publish: serializeJson silently writes
    // as much as fits, and half a JSON document on a retained config topic is a shutter
    // that never appears with nothing anywhere saying why.
    const size_t written = serializeJson(doc, payload, sizeof(payload));
    if (written >= sizeof(payload) - 1) {
      logError("mqtt      : discovery payload for %s does not fit", object);
      continue;
    }
    if (!_mqtt.publish(topic, payload, true)) {
      logError("mqtt      : discovery rejected for %s (%u bytes)", object,
               (unsigned)written);
    }
  }
}

void HaMqtt::publishState(uint8_t remote) {
  const RemoteState &state = _remotes.state(remote);
  char topic[TOPIC_LEN];

  // Retained, unlike the firmware this replaces: without it, a Home Assistant restart
  // leaves every cover blank until somebody presses something.
  if (state.position() != COVER_UNKNOWN) {
    topicCoverState(topic, sizeof(topic), MQTT_DEVICE_ID, remote);
    _mqtt.publish(topic, coverPositionName(state.position()), true);
  }

  // The My switch is momentary: it reports itself off after every press, including the
  // presses it did not cause.
  if (state.last() == SOMFY_MY) {
    topicMyState(topic, sizeof(topic), MQTT_DEVICE_ID, remote);
    _mqtt.publish(topic, "off", true);
  }
}

void HaMqtt::onMessage(const char *topic, const uint8_t *payload, unsigned int length) {
  uint8_t remote = 0;
  if (!remoteFromCommandTopic(topic, MQTT_DEVICE_ID, &remote)) {
    return;   // the subscription is a wildcard, so this is a topic of ours, not a fault
  }

  SomfyCommand command;
  if (!somfyCommandFromText((const char *)payload, length, &command)) {
    logError("mqtt      : remote %u sent a payload that is not a button", remote);
    return;
  }

  _remotes.queue(remote, command);
}
