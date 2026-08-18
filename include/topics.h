#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Every MQTT topic and Home Assistant identifier this firmware publishes, in one pure
// header so `make test` can pin the exact strings.
//
// Pinning them matters more than it looks. Home Assistant derives an entity id from the
// device name and keeps it keyed by unique_id; change either and a dozen covers come back
// as new entities, leaving every automation and dashboard card pointing at nothing. The
// names below are the ones the 2023 sketch published, down to the "wemos" prefix, and the
// tests exist to keep them that way.

#define TOPIC_LEN 64
#define OBJECT_ID_LEN 48
#define DEVICE_NAME_LEN 32
#define DISCOVERY_TOPIC_LEN 96

// "<deviceId>/remote<n>/button" — where commands arrive.
inline void topicCommand(char *out, size_t len, const char *deviceId, uint8_t remote) {
  snprintf(out, len, "%s/remote%u/button", deviceId, (unsigned)remote);
}

// "<deviceId>/+/button" — one subscription instead of one per remote. Twelve SUBSCRIBE
// packets on every reconnect is twelve chances for a flaky broker to lose one, and the
// remote index is in the topic either way.
inline void topicCommandWildcard(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/+/button", deviceId);
}

// The inverse of topicCommand: which remote a message on this topic is for. False for
// anything that is not exactly "<deviceId>/remote<n>/button" — the wildcard above matches
// more than it should, and a mis-parse moves the wrong shutter.
inline bool remoteFromCommandTopic(const char *topic, const char *deviceId, uint8_t *out) {
  const size_t idLength = strlen(deviceId);
  if (strncmp(topic, deviceId, idLength) != 0) {
    return false;
  }

  static const char PREFIX[] = "/remote";
  const char *rest = topic + idLength;
  if (strncmp(rest, PREFIX, sizeof(PREFIX) - 1) != 0) {
    return false;
  }
  rest += sizeof(PREFIX) - 1;

  if (*rest < '0' || *rest > '9') {
    return false;
  }
  unsigned value = 0;
  while (*rest >= '0' && *rest <= '9') {
    value = value * 10 + (unsigned)(*rest - '0');
    if (value > 255) {
      return false;
    }
    rest++;
  }

  if (strcmp(rest, "/button") != 0) {
    return false;
  }
  *out = (uint8_t)value;
  return true;
}

// "<deviceId>/remote<n>/state" — open / closed, retained.
inline void topicCoverState(char *out, size_t len, const char *deviceId, uint8_t remote) {
  snprintf(out, len, "%s/remote%u/state", deviceId, (unsigned)remote);
}

// "<deviceId>/remote<n>/my_state" — the My switch reporting itself back off.
inline void topicMyState(char *out, size_t len, const char *deviceId, uint8_t remote) {
  snprintf(out, len, "%s/remote%u/my_state", deviceId, (unsigned)remote);
}

// "<deviceId>/status" — online / offline, retained, and the broker's last will.
inline void topicAvailability(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/status", deviceId);
}

// "<deviceId><n>" — the Home Assistant device each remote's entities hang off.
inline void deviceIdentifier(char *out, size_t len, const char *deviceId, uint8_t remote) {
  snprintf(out, len, "%s%u", deviceId, (unsigned)remote);
}

// "Somfy Remote<n>". Home Assistant slugifies this into the entity id, so
// cover.somfy_remote3 exists because of this line and nothing else.
inline void deviceName(char *out, size_t len, uint8_t remote) {
  snprintf(out, len, "Somfy Remote%u", (unsigned)remote);
}

// "<deviceId><n>_<suffix>" — cover, my, prog.
inline void uniqueId(char *out, size_t len, const char *deviceId, uint8_t remote,
                     const char *suffix) {
  snprintf(out, len, "%s%u_%s", deviceId, (unsigned)remote, suffix);
}

// "<discoveryPrefix><component>/<objectId>/config", where discoveryPrefix carries its own
// trailing slash. Home Assistant only listens under the prefix its MQTT integration is
// configured with; get it wrong and the bridge publishes happily to nobody.
inline void discoveryTopic(char *out, size_t len, const char *discoveryPrefix,
                           const char *component, const char *objectId) {
  snprintf(out, len, "%s%s/%s/config", discoveryPrefix, component, objectId);
}
