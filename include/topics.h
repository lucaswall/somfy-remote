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
// "wemos" prefix is not a typo — it is load-bearing, and the tests exist to keep it.

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

// "<deviceId>/config" — the retained configuration document. What the device controls,
// rather than what it is doing. Held in Home Assistant so a replacement board recovers it
// instead of needing a rebuild with the right constants compiled in.
inline void topicConfig(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/config", deviceId);
}

// "<deviceId>/code/remote<n>" — the rolling code mirror, retained, one topic per remote so
// a press publishes one small message rather than rewriting all of them.
//
// This is the value that cannot be regenerated: lose it and every motor has to be paired
// by hand again. It lives here so that it survives the board.
inline void topicCode(char *out, size_t len, const char *deviceId, uint8_t remote) {
  snprintf(out, len, "%s/code/remote%u", deviceId, (unsigned)remote);
}

// "<deviceId>/code/+" — one subscription for the mirror, for the same reason the command
// wildcard exists.
inline void topicCodeWildcard(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/code/+", deviceId);
}

// Which remote a mirrored counter belongs to. Exact, like remoteFromCommandTopic: a
// wildcard matches more than it should and a mis-parse would raise the wrong counter.
inline bool remoteFromCodeTopic(const char *topic, const char *deviceId, uint8_t *out) {
  const size_t idLength = strlen(deviceId);
  if (strncmp(topic, deviceId, idLength) != 0) {
    return false;
  }

  static const char PREFIX[] = "/code/remote";
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
  if (*rest != '\0') {
    return false;
  }
  *out = (uint8_t)value;
  return true;
}

// "<deviceId>/control/<6 hex, lower case>" — one learned physical control, retained.
//
// The address is the topic because it is the key: carried in the payload as well it could
// disagree with itself, and this is the copy the broker indexes. Lower case and always six
// digits, so one address has exactly one topic.
//
// One topic per control rather than one document for all of them. The configuration
// document is atomic because a remote count without its addresses is dangerous; a control
// map applied half way is not — one missing entry means one press is not mirrored — and a
// document holding every control in this house would not fit the broker buffer.
inline void topicControl(char *out, size_t len, const char *deviceId, uint32_t address) {
  snprintf(out, len, "%s/control/%06lx", deviceId, (unsigned long)(address & 0xFFFFFFu));
}

inline void topicControlWildcard(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/control/+", deviceId);
}

// Which address a control message is for. Exact, like the other two parsers: the wildcard
// matches more than it should, and these are the RF credentials of a house.
inline bool addressFromControlTopic(const char *topic, const char *deviceId, uint32_t *out) {
  const size_t idLength = strlen(deviceId);
  if (strncmp(topic, deviceId, idLength) != 0) {
    return false;
  }

  static const char PREFIX[] = "/control/";
  const char *rest = topic + idLength;
  if (strncmp(rest, PREFIX, sizeof(PREFIX) - 1) != 0) {
    return false;
  }
  rest += sizeof(PREFIX) - 1;

  uint32_t value = 0;
  uint8_t digits = 0;
  for (; *rest != '\0'; rest++) {
    uint8_t d;
    if (*rest >= '0' && *rest <= '9') {
      d = (uint8_t)(*rest - '0');
    } else if (*rest >= 'a' && *rest <= 'f') {
      d = (uint8_t)(*rest - 'a' + 10);
    } else {
      return false;   // upper case is deliberately refused: one address, one topic
    }
    if (++digits > 6) {
      return false;
    }
    value = (value << 4) | d;
  }
  if (digits != 6) {
    return false;
  }
  *out = value;
  return true;
}

// "<deviceId>/control/<hex>/press" — a press heard from that control, published
// **non-retained**. The only topic on this device that is not retained, and it has to be:
// Home Assistant's MQTT event platform discards a retained payload as a replay, which is
// correct — a button press that happened yesterday is not news to a broker reconnect.
inline void topicControlPress(char *out, size_t len, const char *deviceId,
                              uint32_t address) {
  snprintf(out, len, "%s/control/%06lx/press", deviceId,
           (unsigned long)(address & 0xFFFFFFu));
}

// "<deviceId>/names" — display names, published by Home Assistant rather than by us. The
// device only reads them, and only to show them: nothing here is ever used to key anything.
inline void topicNames(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/names", deviceId);
}

// "<deviceId>/health" — the bridge's own diagnostics, retained. Not a control surface:
// Home Assistant mirrors what the bridge *does*, and this is what it *is*. A rolling code
// that stops advancing while presses are still being logged is the signature of a flash
// that has stopped accepting writes, and that is worth a graph and an alert rather than a
// glance at a web page.
inline void topicHealth(char *out, size_t len, const char *deviceId) {
  snprintf(out, len, "%s/health", deviceId);
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
