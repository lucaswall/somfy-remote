#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "record_store.h"

// The configuration document Home Assistant holds for this bridge, and the rules for
// deciding whose copy wins.
//
// Configuration lives in a retained MQTT message rather than in the firmware so that a
// replacement board recovers it instead of needing a rebuild. It is one document rather
// than a topic per remote because a config change has to apply atomically: a remote count
// and its per-remote entries arriving separately admits a state where the device believes
// in a remote it has no address for.
//
// Every value here is site-specific. The example below uses 0x000000 deliberately — this
// repository is public and the RF address is the credential of a real house.
//
//   { "v": 1, "epoch": 7, "writer": "ui", "hash": 3735928559,
//     "base": "0x000000",
//     "remotes": [ { "i": 0, "addr": "0x000000", "enabled": true, "operational": true,
//                    "travel": 22 } ] }

namespace cfg {

static const uint8_t SCHEMA_VERSION = 1;
static const uint8_t WRITER_LEN = 12;

struct RemoteConfig {
  uint8_t index;
  uint32_t address;   // rs::ADDR_NONE means "derive from base + index"
  bool enabled;
  bool operational;
  uint8_t travelSeconds;   // end to end; 0 means the firmware default
};

// One byte in the store, so the page must refuse what will not survive the round trip.
static const uint8_t MAX_TRAVEL_SECONDS = 255;

struct ConfigDoc {
  uint8_t version = SCHEMA_VERSION;
  uint32_t epoch = 0;
  uint32_t base = 0;
  uint32_t hash = 0;
  char writer[WRITER_LEN] = {0};
  RemoteConfig remotes[rs::MAX_REMOTES] = {};
  uint8_t entries = 0;

  // The count every consumer actually wants: highest configured index + 1, not the number
  // of entries. A sparse document is legal — a removed remote keeps its index reserved and
  // is simply not enabled — and renumbering the survivors would re-key every Home
  // Assistant entity, which is the one thing this firmware must never do.
  uint8_t remoteCount() const {
    uint8_t highest = 0;
    bool any = false;
    for (uint8_t i = 0; i < entries; i++) {
      if (!any || remotes[i].index > highest) {
        highest = remotes[i].index;
        any = true;
      }
    }
    return any ? (uint8_t)(highest + 1) : 0;
  }

  const RemoteConfig *find(uint8_t index) const {
    for (uint8_t i = 0; i < entries; i++) {
      if (remotes[i].index == index) {
        return &remotes[i];
      }
    }
    return nullptr;
  }

  uint32_t addressOf(uint8_t index) const {
    const RemoteConfig *r = find(index);
    if (r != nullptr && r->address != rs::ADDR_NONE) {
      return r->address;
    }
    return base + index;
  }
};

// Content identity, independent of the epoch. Two writers can reach the same epoch with
// different content, and without this the device cannot tell its own document from
// somebody else's.
inline uint32_t contentHash(const ConfigDoc &doc) {
  uint32_t h = 2166136261u;   // FNV-1a
  const uint8_t bytes[4] = {(uint8_t)(doc.base), (uint8_t)(doc.base >> 8),
                            (uint8_t)(doc.base >> 16), (uint8_t)(doc.base >> 24)};
  for (uint8_t i = 0; i < 4; i++) {
    h = (h ^ bytes[i]) * 16777619u;
  }
  for (uint8_t i = 0; i < doc.entries; i++) {
    const RemoteConfig &r = doc.remotes[i];
    const uint8_t f[7] = {r.index,
                          (uint8_t)(r.address), (uint8_t)(r.address >> 8),
                          (uint8_t)(r.address >> 16), (uint8_t)(r.address >> 24),
                          (uint8_t)((r.enabled ? 1 : 0) | (r.operational ? 2 : 0)),
                          r.travelSeconds};
    for (uint8_t k = 0; k < 7; k++) {
      h = (h ^ f[k]) * 16777619u;
    }
  }
  return h;
}

// Addresses are carried as "0x…" strings so a 24-bit value never meets a JSON number's
// float semantics, and so `make check` has a recognisable shape to scan for.
inline uint32_t parseHex(const char *s, uint32_t fallback) {
  if (s == nullptr) {
    return fallback;
  }
  uint32_t v = 0;
  uint8_t digits = 0;
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
  }
  for (; *s != '\0'; s++) {
    uint8_t d;
    if (*s >= '0' && *s <= '9') {
      d = (uint8_t)(*s - '0');
    } else if (*s >= 'a' && *s <= 'f') {
      d = (uint8_t)(*s - 'a' + 10);
    } else if (*s >= 'A' && *s <= 'F') {
      d = (uint8_t)(*s - 'A' + 10);
    } else {
      return fallback;
    }
    if (digits >= 8) {
      return fallback;
    }
    v = (v << 4) | d;
    digits++;
  }
  return digits == 0 ? fallback : v;
}

// parseHex cannot validate, only fall back: it cannot tell an absent value from an
// unparseable one, and "ffffffff" parses to exactly rs::ADDR_NONE — the sentinel meaning
// "derive this from the base". This one refuses rather than guessing, and caps at the six
// hex digits an RTS address actually has.
inline bool parseAddress(const char *s, uint32_t *out) {
  if (s == nullptr || out == nullptr) {
    return false;
  }
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s += 2;
  }
  uint32_t v = 0;
  uint8_t digits = 0;
  for (; *s != '\0'; s++) {
    uint8_t d;
    if (*s >= '0' && *s <= '9') {
      d = (uint8_t)(*s - '0');
    } else if (*s >= 'a' && *s <= 'f') {
      d = (uint8_t)(*s - 'a' + 10);
    } else if (*s >= 'A' && *s <= 'F') {
      d = (uint8_t)(*s - 'A' + 10);
    } else {
      return false;
    }
    if (++digits > 6) {
      return false;   // wider than 24 bits: the transmitter would send the low half only
    }
    v = (v << 4) | d;
  }
  if (digits == 0) {
    return false;
  }
  *out = v;
  return true;
}

// Which indices lose their Home Assistant entities when `to` replaces `from` — one bit
// each. An index that was enabled and is not any more, or that the new count no longer
// reaches. A newly added index sets no bit, and neither does one that stays enabled.
//
// Deliberately not driven by `operational`: a remote flagged not-operational keeps its
// entities on purpose, so the house can still see a shutter it has decided not to drive.
inline uint32_t entitiesToRemove(const ConfigDoc &from, const ConfigDoc &to) {
  uint32_t bits = 0;
  const uint8_t wasCount = from.remoteCount();
  const uint8_t nowCount = to.remoteCount();
  for (uint8_t i = 0; i < wasCount && i < rs::MAX_REMOTES; i++) {
    bool wasEnabled = false;
    for (uint8_t e = 0; e < from.entries; e++) {
      if (from.remotes[e].index == i) {
        wasEnabled = from.remotes[e].enabled;
        break;
      }
    }
    if (!wasEnabled) {
      continue;
    }
    bool isEnabled = false;
    if (i < nowCount) {
      for (uint8_t e = 0; e < to.entries; e++) {
        if (to.remotes[e].index == i) {
          isEnabled = to.remotes[e].enabled;
          break;
        }
      }
    }
    if (!isEnabled) {
      bits |= (uint32_t)1u << i;
    }
  }
  return bits;
}

inline bool parse(const char *json, size_t len, ConfigDoc *out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    return false;
  }
  if (!doc["v"].is<unsigned>() || doc["v"].as<unsigned>() != SCHEMA_VERSION) {
    return false;
  }
  if (!doc["remotes"].is<JsonArrayConst>()) {
    return false;
  }

  ConfigDoc parsed;
  parsed.version = SCHEMA_VERSION;
  parsed.epoch = doc["epoch"] | 0u;
  parsed.base = parseHex(doc["base"] | (const char *)nullptr, 0);
  parsed.hash = doc["hash"] | 0u;
  const char *writer = doc["writer"] | "";
  strncpy(parsed.writer, writer, WRITER_LEN - 1);

  for (JsonObjectConst r : doc["remotes"].as<JsonArrayConst>()) {
    if (parsed.entries >= rs::MAX_REMOTES) {
      return false;   // more remotes than there are counter slots: refuse, do not truncate
    }
    const unsigned index = r["i"] | 0xFFFFu;
    if (index >= rs::MAX_REMOTES) {
      return false;
    }
    RemoteConfig &slot = parsed.remotes[parsed.entries++];
    slot.index = (uint8_t)index;

    // **Fail closed on a mistyped flag**, the way an out-of-range index already does.
    // ArduinoJson's operator| yields the default for any variant that is not the requested
    // type, so "operational":"false" and "operational":0 both read as *true* — granting a
    // remote the one flag whose whole purpose is to stop it transmitting. A document from a
    // template renderer rather than a JSON serialiser is exactly how that happens. An
    // absent key still takes its default; a present key of the wrong type refuses the whole
    // document, which surfaces as the existing "did not parse" error.
    if ((!r["enabled"].isNull() && !r["enabled"].is<bool>()) ||
        (!r["operational"].isNull() && !r["operational"].is<bool>())) {
      return false;
    }

    // Absent means "derive from base". Present means it has to be a real 24-bit address —
    // never silently collapse a typed-but-wrong one into the derived value.
    const char *addr = r["addr"] | (const char *)nullptr;
    slot.address = rs::ADDR_NONE;
    if (addr != nullptr && !parseAddress(addr, &slot.address)) {
      return false;
    }
    slot.enabled = r["enabled"] | true;
    slot.operational = r["operational"] | true;
    const unsigned travel = r["travel"] | 0u;
    slot.travelSeconds = travel > MAX_TRAVEL_SECONDS ? 0 : (uint8_t)travel;
  }

  *out = parsed;
  return true;
}

inline size_t serialise(const ConfigDoc &doc, char *out, size_t cap) {
  JsonDocument j;
  j["v"] = SCHEMA_VERSION;
  j["epoch"] = doc.epoch;
  j["writer"] = doc.writer;
  j["hash"] = contentHash(doc);
  char hex[11];
  snprintf(hex, sizeof(hex), "0x%06lX", (unsigned long)doc.base);
  j["base"] = hex;
  JsonArray arr = j["remotes"].to<JsonArray>();
  for (uint8_t i = 0; i < doc.entries; i++) {
    JsonObject o = arr.add<JsonObject>();
    o["i"] = doc.remotes[i].index;
    if (doc.remotes[i].address != rs::ADDR_NONE) {
      char rhex[11];
      snprintf(rhex, sizeof(rhex), "0x%06lX", (unsigned long)doc.remotes[i].address);
      o["addr"] = rhex;
    }
    o["enabled"] = doc.remotes[i].enabled;
    o["operational"] = doc.remotes[i].operational;
    if (doc.remotes[i].travelSeconds > 0) {
      o["travel"] = doc.remotes[i].travelSeconds;
    }
  }
  // serializeJson() truncates silently when the buffer is short, and a truncated config
  // document is a parse failure at the other end rather than a visible fault here.
  // Returning 0 makes the caller surface it instead of publishing half a document.
  if (measureJson(j) + 1 > cap) {
    return 0;
  }
  return serializeJson(j, out, cap);
}

// What to do with a retained document, given what we already have.
//
// The comparison is `>` and not `>=` deliberately. Adopting on equality re-appends the
// whole configuration on every reconnect, and reconnects are routine by design — but the
// real reason is that `>=` makes a lost update silent: two writers can reach the same
// epoch, and the device would quietly adopt whichever arrived last.
enum Action {
  CFG_NONE,          // nothing anywhere: unconfigured
  CFG_ADOPT,         // remote is newer: adopt and persist
  CFG_VERIFY_ONLY,   // same epoch: compare content, write nothing
  CFG_REPUBLISH,     // local is newer: push it back up
};

inline Action decide(bool haveLocal, uint32_t localEpoch, bool haveRemote,
                     uint32_t remoteEpoch) {
  if (!haveLocal && !haveRemote) {
    return CFG_NONE;
  }
  if (!haveLocal) {
    return CFG_ADOPT;
  }
  if (!haveRemote) {
    return CFG_REPUBLISH;
  }
  if (remoteEpoch > localEpoch) {
    return CFG_ADOPT;
  }
  if (remoteEpoch < localEpoch) {
    return CFG_REPUBLISH;
  }
  return CFG_VERIFY_ONLY;
}

// Projects a document onto the record store's live set. Every remote the document names
// gets an ADDR and FLAGS record; every index it does not name is explicitly cleared to
// ADDR_NONE and no flags. The clear is what stops a removed remote's address override
// surviving compaction forever and being inherited by whatever later takes the index.
inline void project(const ConfigDoc &doc, rs::LiveMap *map) {
  map->put(rs::NS_SCALAR, rs::SCALAR_ADDRESS_BASE, doc.base);
  map->put(rs::NS_SCALAR, rs::SCALAR_REMOTE_COUNT, doc.remoteCount());
  map->put(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH, doc.epoch);
  for (uint8_t i = 0; i < rs::MAX_REMOTES; i++) {
    const RemoteConfig *r = doc.find(i);
    if (r == nullptr) {
      map->put(rs::NS_ADDR, i, rs::ADDR_NONE);
      map->put(rs::NS_FLAGS, i, 0);
      continue;
    }
    map->put(rs::NS_ADDR, i, r->address);
    map->put(rs::NS_FLAGS, i,
             (uint32_t)((r->enabled ? rs::FLAG_ENABLED : 0) |
                        (r->operational ? rs::FLAG_OPERATIONAL : 0) |
                        ((uint32_t)r->travelSeconds << rs::TRAVEL_SECONDS_SHIFT)));
  }
}

// The inverse: what the device believes, ready to be published when the mirror is behind.
inline void fromStore(const rs::LiveMap &map, ConfigDoc *out) {
  ConfigDoc doc;
  doc.base = map.valueOr(rs::NS_SCALAR, rs::SCALAR_ADDRESS_BASE, 0);
  doc.epoch = map.valueOr(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH, 0);
  strncpy(doc.writer, "device", WRITER_LEN - 1);
  const uint8_t count = (uint8_t)map.valueOr(rs::NS_SCALAR, rs::SCALAR_REMOTE_COUNT, 0);
  for (uint8_t i = 0; i < count && i < rs::MAX_REMOTES; i++) {
    RemoteConfig &slot = doc.remotes[doc.entries++];
    slot.index = i;
    slot.address = map.valueOr(rs::NS_ADDR, i, rs::ADDR_NONE);
    const uint32_t flags = map.valueOr(rs::NS_FLAGS, i, 0);
    slot.enabled = (flags & rs::FLAG_ENABLED) != 0;
    slot.operational = (flags & rs::FLAG_OPERATIONAL) != 0;
    slot.travelSeconds =
        (uint8_t)((flags >> rs::TRAVEL_SECONDS_SHIFT) & rs::TRAVEL_SECONDS_MASK);
  }
  doc.hash = contentHash(doc);
  *out = doc;
}

}   // namespace cfg
