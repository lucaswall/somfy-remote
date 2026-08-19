#include "ha_mqtt.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <ESP8266WiFi.h>

#include "build_info.h"
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

// How long to wait after connecting for retained messages before deciding what the device
// knows. Not a blocking wait: the loop keeps running, so OTA and the web UI stay alive.
// Commands are held rather than dropped for the duration.
static const uint32_t CONFIG_WAIT_MS = 3000;

// A mirrored counter further ahead than this is refused rather than adopted. One mistyped
// publish can otherwise span the whole counter space, and unlike a merely stale mirror
// that is not self-correcting — the device would persist the absurd value as local truth
// and the floor rule would then forbid ever lowering it.
static const uint32_t MAX_ADOPT_JUMP = 1000;

// Sized against the worst-case configuration document at thirty remotes, which
// test_config_doc pins below 2048. The default 256 does not fit a discovery payload
// either.
static const uint16_t MQTT_BUFFER = 2048;

// Diagnostics are retained, so a slow cadence still leaves Home Assistant with a current
// value; this only decides how quickly a developing fault becomes visible.
static const uint32_t HEALTH_PUBLISH_MS = 60000;

// snprintf truncates silently, and two truncated topics are one topic: remote 2 and
// remote 21 would share a command topic and move together. The longest this firmware
// builds is "<id>/remote29/my_state", so fail the build rather than the installation.
static_assert(sizeof(MQTT_DEVICE_ID) + sizeof("/remote29/my_state") - 1 <= TOPIC_LEN,
              "MQTT_DEVICE_ID is too long: see TOPIC_LEN in include/topics.h");

// OBJECT_ID_LEN is the tighter of the two: "<id>29_cover" has to fit as well, and a
// truncated unique_id is worse than a truncated topic — two remotes would collide on one
// Home Assistant entity rather than merely on one topic.
static_assert(sizeof(MQTT_DEVICE_ID) + sizeof("29_cover") - 1 <= OBJECT_ID_LEN,
              "MQTT_DEVICE_ID is too long: see OBJECT_ID_LEN in include/topics.h");

// What the board already believes, read back out of the store. Without this the device
// starts every boot claiming to have no configuration, re-adopts the retained document it
// already applied, and cannot compare epochs meaningfully — its own copy would always
// look older than anyone else's.
void HaMqtt::loadConfigFromStore() {
  _loaded = true;
  if (!_store.has(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH)) {
    return;
  }
  cfg::fromStore(_store.map(), &_config);
  _haveConfig = true;
}

void HaMqtt::loop() {
  if (!_loaded) {
    loadConfigFromStore();
  }
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

  // Retained messages arrive during this window. Deciding early would mean reconciling
  // against a mirror that had not finished arriving, and adopting a counter that is
  // merely late looks exactly like adopting one that is absent.
  if (_reconciling) {
    // Measured from the start, not against a precomputed deadline: elapsed() subtracts
    // unsigned to survive the millis() rollover, so a timestamp in the future underflows
    // to a huge interval and reads as already expired.
    if (!elapsed(millis(), _reconcileStart, CONFIG_WAIT_MS)) {
      return;
    }
    finishReconcile();
    return;
  }

  // A configuration can arrive at any time, not only during the boot window — it is how a
  // blank board is told what it controls, and whoever seeds it may well do so after the
  // board has already given up waiting.
  if (_haveStaged && (!_haveConfig || _staged.epoch > _config.epoch)) {
    reconcileConfig();
    reconcileCounters();
    for (uint8_t i = 0; i < _remotes.count(); i++) {
      if (_remotes.enabled(i)) {
        publishDiscovery(i);
        publishState(i);
      }
    }
  }

  // Publish on any change, whoever caused it — a command from Home Assistant or a press
  // on the web page.
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    if (_remotes.state(i).version() != _publishedVersion[i]) {
      _publishedVersion[i] = _remotes.state(i).version();
      publishState(i);
    }
    publishCounter(i);
  }

  applyHeardPresses();

  if (elapsed(millis(), _lastHealth, HEALTH_PUBLISH_MS)) {
    _lastHealth = millis();
    publishHealth();
  }
}

// A press somebody made on a handheld. This is the whole point of listening: until now
// every cover in Home Assistant has been a report of what this bridge believes it sent, and
// a shutter opened by hand stayed closed on the dashboard until somebody used the app.
//
// Nothing here transmits, queues a command, or touches a rolling code. A foreign counter
// and one of ours live in different address spaces and must never meet.
void HaMqtt::applyHeardPresses() {
  SomfyPress press;
  while (_receiver.takePress(&press)) {
    const ctl::Control *control = _controls.find(press.address);
    if (control == nullptr) {
      continue;   // heard, recorded as a sighting, and waiting for a human to name it
    }

    // Not retained, and it must not be: Home Assistant discards a retained payload on an
    // event topic as a replay, which is right — a button pressed yesterday is not news to
    // a broker reconnect.
    char topic[TOPIC_LEN], payload[64];
    topicControlPress(topic, sizeof(topic), MQTT_DEVICE_ID, press.address);
    snprintf(payload, sizeof(payload), "{\"event_type\":\"%x\",\"code\":%u}",
             press.command, press.rollingCode);
    _mqtt.publish(topic, payload, false);

    uint8_t applied = 0;
    for (uint8_t i = 0; i < rs::MAX_REMOTES; i++) {
      if ((control->drives & ((uint32_t)1u << i)) == 0) {
        continue;
      }
      if (_remotes.observe(i, press.command)) {
        applied++;
      }
    }
    logLine("heard     : %s %s, %u cover(s) updated", control->name,
            somfyCommandName((SomfyCommand)press.command), applied);
  }
}

// Names a control and publishes it retained, in that order, so the page shows what the
// device believes even if the broker refuses. False means the retained copy did not land —
// the RAM map is authoritative for reads only, and with no broker a save has nowhere
// durable to go.
bool HaMqtt::saveControl(const ctl::Control &control) {
  if (!_controls.set(control)) {
    logError("mqtt      : no room for another control, %u already", _controls.count());
    return false;
  }

  char topic[TOPIC_LEN], payload[ctl::PAYLOAD_LEN];
  topicControl(topic, sizeof(topic), MQTT_DEVICE_ID, control.address);
  const size_t written = ctl::serialise(control, payload, sizeof(payload));
  if (written == 0 || !_mqtt.publish(topic, payload, true)) {
    logError("mqtt      : control \"%s\" was not saved to the broker", control.name);
    return false;
  }
  publishControlDiscovery(control);
  logLine("control   : \"%s\" saved", control.name);
  return true;
}

bool HaMqtt::forgetControl(uint32_t address) {
  char topic[TOPIC_LEN];
  topicControl(topic, sizeof(topic), MQTT_DEVICE_ID, address);
  if (!_mqtt.publish(topic, "", true)) {
    return false;
  }
  publishControlRemoval(address);
  _controls.remove(address);
  return true;
}

// The floor, enforced on the way out. A press only ever raises a counter, so a publish
// that would lower the retained value means something is wrong — a stale local store, a
// remote that was re-added, a mirror that has moved on — and sending it would destroy the
// one copy that survives the board.
void HaMqtt::publishCounter(uint8_t remote) {
  const uint32_t value = _remotes.counter(remote);
  if (!_remotes.hasCounter(remote) || value <= _mirrorSeen[remote]) {
    return;
  }
  char topic[TOPIC_LEN], payload[12];
  topicCode(topic, sizeof(topic), MQTT_DEVICE_ID, remote);
  snprintf(payload, sizeof(payload), "%lu", (unsigned long)value);
  if (_mqtt.publish(topic, payload, true)) {
    _mirrorSeen[remote] = value;
  }
}

bool HaMqtt::connect() {
  char availability[TOPIC_LEN];
  topicAvailability(availability, sizeof(availability), MQTT_DEVICE_ID);

  _mqtt.setServer(MQTT_HOST, MQTT_PORT);
  _mqtt.setBufferSize(MQTT_BUFFER);
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

  // Availability is deliberately not published here. Announcing the bridge as online
  // before it has decided whose counters win leaves a window where Home Assistant believes
  // commands will be honoured — and the command topic is not subscribed yet either, so
  // they would simply be lost. Both happen at the end of finishReconcile().
  char topic[TOPIC_LEN];
  topicConfig(topic, sizeof(topic), MQTT_DEVICE_ID);
  _mqtt.subscribe(topic);
  topicCodeWildcard(topic, sizeof(topic), MQTT_DEVICE_ID);
  _mqtt.subscribe(topic);
  topicNames(topic, sizeof(topic), MQTT_DEVICE_ID);
  _mqtt.subscribe(topic);
  // The learned controls. Retained and replayed on subscribe, which is what lets a
  // replacement board recover a map somebody spent an hour walking a house to build.
  topicControlWildcard(topic, sizeof(topic), MQTT_DEVICE_ID);
  _mqtt.subscribe(topic);
  // Our own retained cover state, read back. The device infers position from what it
  // transmitted and holds it in RAM, so every reboot and every OTA used to throw it away —
  // while Home Assistant kept it. Reading it back is the same trick as the counters: the
  // durable copy lives off the board.
  snprintf(topic, sizeof(topic), "%s/+/state", MQTT_DEVICE_ID);
  _mqtt.subscribe(topic);

  _haveStaged = false;
  for (uint8_t i = 0; i < rs::MAX_REMOTES; i++) {
    _haveMirror[i] = false;
  }
  _reconciling = true;
  _reconcileStart = millis();
  _remotes.hold(true);

  logLine("mqtt      : connected to %s as %s, waiting %lums for retained state", MQTT_HOST,
          MQTT_USER, (unsigned long)CONFIG_WAIT_MS);
  return true;
}

void HaMqtt::finishReconcile() {
  _reconciling = false;

  reconcileConfig();
  reconcileCounters();

  // Only now does the device accept commands: the counters are settled, so a press cannot
  // be answered from a store that is about to be raised.
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
  //
  // Disabled remotes are skipped, or a broker restart would resurrect an entity the web
  // UI has just removed.
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    if (!_remotes.enabled(i)) {
      continue;
    }
    publishDiscovery(i);
    publishState(i);
    publishCounter(i);
  }

  publishBridgeDiscovery();
  publishHealth();

  char availability[TOPIC_LEN];
  topicAvailability(availability, sizeof(availability), MQTT_DEVICE_ID);
  _mqtt.publish(availability, "online", true);

  _remotes.hold(false);
  logLine("mqtt      : ready, %u remotes announced", _remotes.count());
}

// The whole recovery story, in one loop. On a board that has been running, local is ahead
// and the mirror is corrected upward. On a replacement board local is empty and the mirror
// supplies everything — and the adopted value is *persisted*, not merely believed, because
// a value held only in RAM is gone at the next reboot and the device would resume sending
// codes from years ago.
void HaMqtt::reconcileCounters() {
  uint8_t adopted = 0, corrected = 0, refused = 0, missing = 0;

  for (uint8_t i = 0; i < _remotes.count(); i++) {
    uint32_t effective = 0;
    const rs::Reconcile action =
        rs::reconcile(_remotes.hasCounter(i), _remotes.counter(i), _haveMirror[i],
                      _mirror[i], MAX_ADOPT_JUMP, &effective);

    if (_haveMirror[i] && _mirror[i] > _mirrorSeen[i]) {
      _mirrorSeen[i] = _mirror[i];
    }

    switch (action) {
      case rs::REC_ADOPT:
        if (_remotes.adoptCounter(i, effective)) {
          adopted++;
        }
        break;
      case rs::REC_KEEP_PUBLISH:
        publishCounter(i);
        corrected++;
        break;
      case rs::REC_REFUSE_JUMP:
        logError("mqtt      : remote %u mirror %lu is %lu ahead of %lu — refused",
                 i, (unsigned long)_mirror[i],
                 (unsigned long)(_mirror[i] - _remotes.counter(i)),
                 (unsigned long)_remotes.counter(i));
        refused++;
        break;
      case rs::REC_NONE:
        missing++;
        break;
      case rs::REC_KEEP_QUIET:
        break;
    }
  }

  if (adopted || corrected || refused || missing) {
    logLine("mqtt      : counters %u adopted, %u corrected upward, %u refused, %u missing",
            adopted, corrected, refused, missing);
  }
  if (missing > 0) {
    logError("mqtt      : %u remote(s) have no rolling code and cannot transmit", missing);
  }
}

void HaMqtt::reconcileConfig() {
  const cfg::Action action =
      cfg::decide(_haveConfig, _config.epoch, _haveStaged, _staged.epoch);

  switch (action) {
    case cfg::CFG_ADOPT: {
      rs::LiveMap projected;
      cfg::project(_staged, &projected);
      bool ok = true;
      // Diff-then-append: only records that actually change cost a slot, so a one-remote
      // edit does not rewrite the whole configuration.
      for (uint8_t i = 0; i < projected.count(); i++) {
        const rs::Entry &e = projected.at(i);
        if (_store.valueOr(e.ns, e.id, 0xFFFFFFFEu) == e.value) {
          continue;
        }
        if (!_store.put(e.ns, e.id, e.value)) {
          ok = false;
          break;
        }
      }
      // The epoch is written last and alone. A power loss part-way through the records
      // above leaves the previous epoch committed, so replay sees the old configuration
      // rather than half of the new one — which matters because a half-applied config
      // would key some remotes to the wrong RF address.
      const bool epochUnchanged =
          _store.valueOr(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH, 0xFFFFFFFFu) ==
          _staged.epoch;
      if (ok && (epochUnchanged ||
                 _store.put(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH, _staged.epoch))) {
        _config = _staged;
        _haveConfig = true;
        logLine("mqtt      : config epoch %lu adopted from %s, %u remotes",
                (unsigned long)_config.epoch, _config.writer, _config.remoteCount());
      } else {
        logError("mqtt      : config epoch %lu could not be persisted",
                 (unsigned long)_staged.epoch);
      }
      break;
    }
    case cfg::CFG_REPUBLISH:
      publishConfigDocument();
      break;
    case cfg::CFG_VERIFY_ONLY:
      if (cfg::contentHash(_staged) != cfg::contentHash(_config)) {
        logError("mqtt      : config epoch %lu differs from ours — writer '%s'. "
                 "Not adopted; raise the epoch to win.",
                 (unsigned long)_staged.epoch, _staged.writer);
      }
      break;
    case cfg::CFG_NONE:
      logError("mqtt      : no configuration anywhere — controlling nothing");
      break;
  }
}

void HaMqtt::publishConfigDocument() {
  if (!_haveConfig) {
    return;
  }
  char topic[TOPIC_LEN];
  topicConfig(topic, sizeof(topic), MQTT_DEVICE_ID);
  static char payload[MQTT_BUFFER];
  const size_t n = cfg::serialise(_config, payload, sizeof(payload));
  if (n == 0) {
    logError("mqtt      : config document does not fit %u bytes", (unsigned)MQTT_BUFFER);
    return;
  }
  if (!_mqtt.publish(topic, payload, true)) {
    logError("mqtt      : config publish rejected (%u bytes)", (unsigned)n);
  }
}

bool HaMqtt::applyConfig(const cfg::ConfigDoc &doc, const char *writer) {
  cfg::ConfigDoc next = doc;
  next.epoch = (_haveConfig ? _config.epoch : 0) + 1;
  strncpy(next.writer, writer, cfg::WRITER_LEN - 1);
  next.writer[cfg::WRITER_LEN - 1] = '\0';

  rs::LiveMap projected;
  cfg::project(next, &projected);
  for (uint8_t i = 0; i < projected.count(); i++) {
    const rs::Entry &e = projected.at(i);
    if (e.ns == rs::NS_SCALAR && e.id == rs::SCALAR_CONFIG_EPOCH) {
      continue;   // epoch last
    }
    if (_store.valueOr(e.ns, e.id, 0xFFFFFFFEu) == e.value) {
      continue;
    }
    if (!_store.put(e.ns, e.id, e.value)) {
      logError("mqtt      : config could not be persisted");
      return false;
    }
  }
  if (!_store.put(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH, next.epoch)) {
    logError("mqtt      : config epoch could not be persisted");
    return false;
  }

  _config = next;
  _haveConfig = true;
  publishConfigDocument();
  for (uint8_t i = 0; i < _remotes.count(); i++) {
    if (_remotes.enabled(i)) {
      publishDiscovery(i);
    }
  }
  logLine("mqtt      : config epoch %lu written by %s, %u remotes",
          (unsigned long)next.epoch, next.writer, next.remoteCount());
  return true;
}

const char *HaMqtt::nameOf(uint8_t remote) const {
  return remote < rs::MAX_REMOTES ? _names[remote] : "";
}

// An empty retained payload on a discovery topic is how MQTT discovery says "forget this".
// The two retained state topics go with it, or they persist into every nightly backup for
// an entity nothing will ever republish.
void HaMqtt::publishDiscoveryRemoval(uint8_t remote) {
  static const char *const SUFFIX[3] = {"prog", "my", "cover"};
  static const char *const COMPONENT[3] = {"button", "switch", "cover"};
  char object[OBJECT_ID_LEN], topic[DISCOVERY_TOPIC_LEN];
  for (uint8_t i = 0; i < 3; i++) {
    uniqueId(object, sizeof(object), MQTT_DEVICE_ID, remote, SUFFIX[i]);
    discoveryTopic(topic, sizeof(topic), HA_DISCOVERY_PREFIX, COMPONENT[i], object);
    _mqtt.publish(topic, "", true);
  }
  char state[TOPIC_LEN];
  topicCoverState(state, sizeof(state), MQTT_DEVICE_ID, remote);
  _mqtt.publish(state, "", true);
  topicMyState(state, sizeof(state), MQTT_DEVICE_ID, remote);
  _mqtt.publish(state, "", true);
  logLine("mqtt      : remote %u entities removed from Home Assistant", remote);
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
  // Three entities per remote: the cover, the My switch, and the rolling code as a
  // diagnostic sensor.
  //
  // Prog is deliberately not among them. Home Assistant mirrors what the bridge *does* —
  // open, stop, close — not how it is configured, and pairing a remote to a motor is
  // configuration. It lives on the device's own settings page, behind a password, next to
  // the address it pairs. A one-tap unconfirmed button in a dashboard is the wrong home
  // for the one action here that cannot be undone by pressing something else.
  //
  // Not publishing a button was never the whole of that, though: the command topic is a
  // control surface too, and it accepted "Prog" from any broker client until onMessage()
  // was taught to refuse it.
  for (uint8_t entity = 0; entity < 3; entity++) {
    JsonDocument doc;
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = identifier;
    device["name"] = name;
    device["model"] = "Wemos Somfy Remote";
    device["manufacturer"] = "MLCM Tech";
    device["sw_version"] = BUILD_STAMP;
    device["configuration_url"] = url;

    doc["availability_topic"] = availability;
    doc["command_topic"] = command;

    const char *component = nullptr;
    switch (entity) {
      case 0: {
        // The rolling code, as a diagnostic. It is the number that cannot be regenerated,
        // and a counter that stops advancing while presses are still logged is exactly
        // what a flash that has stopped accepting writes looks like from outside.
        component = "sensor";
        uniqueId(object, sizeof(object), MQTT_DEVICE_ID, remote, "code");
        char code[TOPIC_LEN];
        topicCode(code, sizeof(code), MQTT_DEVICE_ID, remote);
        doc["name"] = "Rolling code";
        doc["state_topic"] = code;
        doc["entity_category"] = "diagnostic";
        doc["state_class"] = "total_increasing";
        doc["icon"] = "mdi:counter";
        doc.remove("command_topic");   // nothing to command: this one only reports
        break;
      }
      case 1:
        // My as a switch rather than a button, because Google Home does not expose
        // buttons usefully. Both payloads are the same press; state_on and state_off are
        // what make it fall back to off afterwards rather than sitting permanently
        // unknown.
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
        // Without this Home Assistant treats them as generic covers: generic icon, generic
        // open/closed vocabulary. They are shutters.
        doc["device_class"] = "shutter";
        // RTS is one-way. Home Assistant shows both buttons at all times rather than
        // hiding the one it thinks is redundant, because what it thinks may be wrong.
        //
        // `optimistic` is the documented key. Do not reach for `assumed_state`: that is
        // what the attribute is called on the entity, but it is not in the MQTT cover
        // schema and discovery drops it silently — an entity that reports the attribute
        // is not evidence the key worked, because a customize rule sets the same thing.
        //
        // State still arrives on the state topic either way: optimistic only means the
        // entity moves on the command instead of waiting for us.
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

// One extra Home Assistant device for the bridge itself, carrying what the store is doing.
// Separate from the twelve remotes because it is not about any one shutter.
void HaMqtt::publishBridgeDiscovery() {
  char availability[TOPIC_LEN], health[TOPIC_LEN];
  topicAvailability(availability, sizeof(availability), MQTT_DEVICE_ID);
  topicHealth(health, sizeof(health), MQTT_DEVICE_ID);
  char url[48];
  snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());

  static const char *const KEY[4] = {"free", "spent", "sector", "heap"};
  static const char *const LABEL[4] = {"Store free slots", "Store faults", "Store sector",
                                       "Free heap"};
  static const char *const ICON[4] = {"mdi:database", "mdi:alert-circle-outline",
                                      "mdi:database-marker", "mdi:memory"};

  char object[OBJECT_ID_LEN], topic[DISCOVERY_TOPIC_LEN], payload[PAYLOAD_LEN];
  for (uint8_t i = 0; i < 4; i++) {
    JsonDocument doc;
    JsonObject device = doc["device"].to<JsonObject>();
    device["identifiers"][0] = MQTT_DEVICE_ID;
    device["name"] = "Somfy Bridge";
    device["model"] = "Wemos Somfy Remote";
    device["manufacturer"] = "MLCM Tech";
    device["sw_version"] = BUILD_STAMP;
    device["configuration_url"] = url;

    doc["availability_topic"] = availability;
    doc["state_topic"] = health;
    doc["name"] = LABEL[i];
    doc["icon"] = ICON[i];
    doc["entity_category"] = "diagnostic";
    char tmpl[40];
    snprintf(tmpl, sizeof(tmpl), "{{ value_json.%s }}", KEY[i]);
    doc["value_template"] = tmpl;

    snprintf(object, sizeof(object), "%s_%s", MQTT_DEVICE_ID, KEY[i]);
    doc["unique_id"] = object;
    discoveryTopic(topic, sizeof(topic), HA_DISCOVERY_PREFIX, "sensor", object);
    const size_t written = serializeJson(doc, payload, sizeof(payload));
    if (written < sizeof(payload) - 1) {
      _mqtt.publish(topic, payload, true);
    }
  }

  // The one that matters enough to be a problem rather than a number.
  JsonDocument doc;
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"][0] = MQTT_DEVICE_ID;
  device["name"] = "Somfy Bridge";
  doc["availability_topic"] = availability;
  doc["state_topic"] = health;
  doc["name"] = "Store degraded";
  doc["entity_category"] = "diagnostic";
  doc["device_class"] = "problem";
  doc["value_template"] = "{{ 'ON' if value_json.degraded else 'OFF' }}";
  snprintf(object, sizeof(object), "%s_degraded", MQTT_DEVICE_ID);
  doc["unique_id"] = object;
  discoveryTopic(topic, sizeof(topic), HA_DISCOVERY_PREFIX, "binary_sensor", object);
  const size_t written = serializeJson(doc, payload, sizeof(payload));
  if (written < sizeof(payload) - 1) {
    _mqtt.publish(topic, payload, true);
  }
}

// One Home Assistant event entity per named control, so a wall button becomes something an
// automation can trigger on. Fifteen physical controls in this house, several buttons each:
// that is arguably worth more than the cover state this feature was built for.
//
// Three constraints come from the platform and are not negotiable. The state topic must be
// non-retained (a retained payload is discarded as a replay), `event_types` is required,
// and the payload must carry an `event_type` drawn from it. The list is every command
// nibble the protocol defines rather than the four this firmware transmits — a handheld
// sends My+Up, My+Down, Up+Down, Sun and Flag too, and an entity that dropped them would
// throw away exactly what makes a multi-button remote interesting.
void HaMqtt::publishControlDiscovery(const ctl::Control &control) {
  static const char *const NIBBLES[9] = {"1", "2", "3", "4", "5", "6", "8", "9", "a"};

  char availability[TOPIC_LEN], press[TOPIC_LEN];
  topicAvailability(availability, sizeof(availability), MQTT_DEVICE_ID);
  topicControlPress(press, sizeof(press), MQTT_DEVICE_ID, control.address);

  JsonDocument doc;
  JsonObject device = doc["device"].to<JsonObject>();
  device["identifiers"][0] = MQTT_DEVICE_ID;
  device["name"] = "Somfy Bridge";

  doc["availability_topic"] = availability;
  doc["state_topic"] = press;
  doc["name"] = control.name;
  doc["icon"] = "mdi:remote";
  JsonArray types = doc["event_types"].to<JsonArray>();
  for (uint8_t i = 0; i < 9; i++) {
    types.add(NIBBLES[i]);
  }

  char object[OBJECT_ID_LEN], topic[DISCOVERY_TOPIC_LEN], payload[PAYLOAD_LEN];
  snprintf(object, sizeof(object), "%s_ctl_%06lx", MQTT_DEVICE_ID,
           (unsigned long)(control.address & 0xFFFFFFu));
  doc["unique_id"] = object;
  discoveryTopic(topic, sizeof(topic), HA_DISCOVERY_PREFIX, "event", object);
  const size_t written = serializeJson(doc, payload, sizeof(payload));
  if (written < sizeof(payload) - 1) {
    _mqtt.publish(topic, payload, true);
  }
}

void HaMqtt::publishControlRemoval(uint32_t address) {
  char object[OBJECT_ID_LEN], topic[DISCOVERY_TOPIC_LEN];
  snprintf(object, sizeof(object), "%s_ctl_%06lx", MQTT_DEVICE_ID,
           (unsigned long)(address & 0xFFFFFFu));
  discoveryTopic(topic, sizeof(topic), HA_DISCOVERY_PREFIX, "event", object);
  _mqtt.publish(topic, "", true);
}

void HaMqtt::publishHealth() {
  char topic[TOPIC_LEN], payload[192];
  topicHealth(topic, sizeof(topic), MQTT_DEVICE_ID);
  snprintf(payload, sizeof(payload),
           "{\"free\":%u,\"spent\":%u,\"sector\":\"%c\",\"heap\":%u,"
           "\"degraded\":%s,\"epoch\":%lu}",
           _store.freeSlots(), _store.spent(), _store.activeName(), ESP.getFreeHeap(),
           _store.degraded() ? "true" : "false", (unsigned long)_config.epoch);
  _mqtt.publish(topic, payload, true);
}

void HaMqtt::publishState(uint8_t remote) {
  const RemoteState &state = _remotes.state(remote);
  char topic[TOPIC_LEN];

  // Retained, so a Home Assistant restart does not leave every cover blank until somebody
  // presses something.
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

// Nothing is published from here. PubSubClient hands the callback pointers into the very
// buffer publish() writes through, so a publish inside a callback can overwrite the
// payload being read. Everything received is staged, and the sequence that acts on it
// runs from the loop.
void HaMqtt::onMessage(const char *topic, const uint8_t *payload, unsigned int length) {
  uint8_t remote = 0;

  if (remoteFromCommandTopic(topic, MQTT_DEVICE_ID, &remote)) {
    SomfyCommand command;
    if (!somfyCommandFromText((const char *)payload, length, &command)) {
      logError("mqtt      : remote %u sent a payload that is not a button", remote);
      return;
    }
    // Prog enrols or *unenrols* an emulated remote at a motor, and unenrolling costs a walk
    // to that shutter with a working handheld. Both web paths refuse it here — /api/send
    // rejects it outright and the settings page confirms first — and this one accepted it
    // from any client that can reach the broker. Two lines in Developer Tools should not be
    // able to do the one thing in this project that cannot be undone by pressing something
    // else.
    if (command == SOMFY_PROG) {
      logError("mqtt      : remote %u Prog refused — pairing is a settings-page operation",
               remote);
      return;
    }
    _remotes.queue(remote, command);
    return;
  }

  // "<id>/remote<n>/state" — our own retained publication, coming back to us on subscribe.
  {
    const size_t idLength = strlen(MQTT_DEVICE_ID);
    if (strncmp(topic, MQTT_DEVICE_ID, idLength) == 0) {
      char expectedState[TOPIC_LEN];
      for (uint8_t i = 0; i < _remotes.count(); i++) {
        topicCoverState(expectedState, sizeof(expectedState), MQTT_DEVICE_ID, i);
        if (strcmp(topic, expectedState) != 0) {
          continue;
        }
        if (length == 4 && memcmp(payload, "open", 4) == 0) {
          _remotes.restoreState(i, COVER_OPEN);
        } else if (length == 6 && memcmp(payload, "closed", 6) == 0) {
          _remotes.restoreState(i, COVER_CLOSED);
        }
        return;
      }
    }
  }

  if (remoteFromCodeTopic(topic, MQTT_DEVICE_ID, &remote)) {
    if (remote >= rs::MAX_REMOTES) {
      return;
    }
    // A bare decimal integer and nothing else. A payload that is not one is treated as
    // absent rather than as zero: adopting a zero would be a counter moving backwards to
    // the beginning, which is the one direction that cannot be undone.
    uint32_t value = 0;
    if (length == 0 || length > 10) {
      logError("mqtt      : remote %u mirror payload rejected", remote);
      return;
    }
    for (unsigned int i = 0; i < length; i++) {
      if (payload[i] < '0' || payload[i] > '9') {
        logError("mqtt      : remote %u mirror payload is not a number", remote);
        return;
      }
      value = value * 10 + (uint32_t)(payload[i] - '0');
    }
    _mirror[remote] = value;
    _haveMirror[remote] = true;
    return;
  }

  uint32_t address = 0;
  if (addressFromControlTopic(topic, MQTT_DEVICE_ID, &address)) {
    if (length == 0) {
      _controls.remove(address);   // an empty retained payload is how MQTT says "forget"
      return;
    }
    ctl::Control control;
    if (!ctl::parse((const char *)payload, length, address, &control)) {
      logError("mqtt      : a control payload did not parse");
      return;
    }
    if (!_controls.set(control)) {
      logError("mqtt      : no room for another control, %u already", _controls.count());
    }
    return;
  }

  char expected[TOPIC_LEN];
  topicConfig(expected, sizeof(expected), MQTT_DEVICE_ID);
  if (strcmp(topic, expected) == 0) {
    if (length == 0) {
      return;   // a cleared config topic is not a config
    }
    if (cfg::parse((const char *)payload, length, &_staged)) {
      _haveStaged = true;
    } else {
      logError("mqtt      : retained config document did not parse");
    }
    return;
  }

  topicNames(expected, sizeof(expected), MQTT_DEVICE_ID);
  if (strcmp(topic, expected) == 0) {
    JsonDocument doc;
    if (deserializeJson(doc, payload, length) != DeserializationError::Ok) {
      logError("mqtt      : names document did not parse");
      return;
    }
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
      const int index = atoi(kv.key().c_str());
      if (index < 0 || index >= (int)rs::MAX_REMOTES) {
        continue;
      }
      const char *value = kv.value().as<const char *>();
      if (value == nullptr) {
        continue;
      }
      strncpy(_names[index], value, sizeof(_names[0]) - 1);
      _names[index][sizeof(_names[0]) - 1] = '\0';
    }
    return;
  }
}
