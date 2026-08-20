#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "record_store.h"

// Which physical control is which, and what each one drives.
//
// A captured frame says nothing about which shutter moved, and RTS is one-way so nothing
// ever will. This map is declared by a human, one control at a time.
//
// One retained MQTT topic per control rather than one document: a control map applied
// half-way is harmless, unlike the configuration, and a document large enough for a real
// house would not fit the broker buffer.
//
// Not persisted to flash. The record store keys by a byte and carries one u32, so it cannot
// express an address-keyed entity with a name at all.

namespace ctl {

static const uint8_t NAME_LEN = 40;

// A dozen wall buttons plus two or three multi-channel handhelds. 1 KB total.
static const uint8_t MAX_CONTROLS = 32;

// A name reaches three hand-built JSON documents and the log ring, and /log is served as
// text/plain with no password — so an embedded newline forges a log line. Refusing is what
// covers that; stripping the bytes silently would not, and would also alter what somebody
// typed. Bytes at or above 0x80 are UTF-8 continuation bytes, not control characters:
// accented names must keep working.
inline bool nameIsAcceptable(const char *name) {
  if (name == nullptr || *name == '\0') {
    return false;
  }
  for (const char *p = name; *p != '\0'; p++) {
    const unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\' || c < 0x20 || c == 0x7F) {
      return false;
    }
  }
  return true;
}

// The longest name, every index set, and the JSON around them.
static const size_t PAYLOAD_LEN = 224;

struct Control {
  uint32_t address;
  uint32_t drives;   // bitmask over remote indices; rs::MAX_REMOTES is 30, so a u32 fits
  uint32_t lastMs;   // runtime only: when it was last heard, so the list can order by it
  char name[NAME_LEN];
};

// The address comes from the topic, not the payload: carried twice it could disagree with
// itself.
//
// `d` is checked against the static bound only. Whether an index currently *exists* is a
// question about the configuration document, which arrives after these retained topics on a
// cold boot — answering it here would empty every drives set on a blank replacement board.
inline bool parse(const char *json, size_t len, uint32_t address, Control *out) {
  if (len == 0) {
    return false;   // an empty retained payload means "forget this", not "a nameless one"
  }

  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) {
    return false;
  }
  if (!doc["n"].is<const char *>()) {
    return false;
  }

  const char *name = doc["n"].as<const char *>();
  if (strlen(name) >= NAME_LEN) {
    return false;   // refused, not cut: a truncated name still looks like a name
  }

  Control parsed = {};
  parsed.address = address;
  strncpy(parsed.name, name, NAME_LEN - 1);

  for (JsonVariantConst index : doc["d"].as<JsonArrayConst>()) {
    const unsigned value = index | 0xFFFFu;
    if (value < rs::MAX_REMOTES) {
      parsed.drives |= (uint32_t)1u << value;
    }
  }

  *out = parsed;
  return true;
}

// Returns 0 rather than truncating: half a payload is a parse failure at the far end.
inline size_t serialise(const Control &control, char *out, size_t cap) {
  JsonDocument j;
  j["n"] = control.name;
  JsonArray drives = j["d"].to<JsonArray>();
  for (uint8_t i = 0; i < rs::MAX_REMOTES; i++) {
    if ((control.drives & ((uint32_t)1u << i)) != 0) {
      drives.add(i);
    }
  }
  if (measureJson(j) + 1 > cap) {
    return 0;
  }
  return serializeJson(j, out, cap);
}

// Last write wins per address: each control is its own retained topic.
class ControlMap {
 public:
  const Control *find(uint32_t address) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_controls[i].address == address) {
        return &_controls[i];
      }
    }
    return nullptr;
  }

  // False means full — refused rather than dropped, since somebody walked a house to name it.
  bool set(const Control &control) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_controls[i].address == control.address) {
        const uint32_t heard = _controls[i].lastMs;
        _controls[i] = control;
        if (_controls[i].lastMs == 0) {
          _controls[i].lastMs = heard;   // a rename must not forget when it was last used
        }
        return true;
      }
    }
    if (_count >= MAX_CONTROLS) {
      return false;
    }
    _controls[_count++] = control;
    return true;
  }

  bool remove(uint32_t address) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_controls[i].address == address) {
        _controls[i] = _controls[--_count];
        return true;
      }
    }
    return false;
  }

  uint8_t count() const { return _count; }
  const Control &at(uint8_t i) const { return _controls[i]; }

  void heard(uint32_t address, uint32_t nowMs) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_controls[i].address == address) {
        _controls[i].lastMs = nowMs;
        return;
      }
    }
  }

 private:
  Control _controls[MAX_CONTROLS] = {};
  uint8_t _count = 0;
};

}   // namespace ctl
