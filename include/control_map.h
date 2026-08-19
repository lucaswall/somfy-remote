#pragma once

#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "record_store.h"

// Which physical control is which, and what each one drives.
//
// A captured frame carries an address, a rolling code and a command. Nothing in it says
// which shutter moved, and because RTS is one-way nothing ever will — the motors never
// answer, so there is no ground truth to infer from. This map is therefore *declared by a
// human*, one control at a time, on the device's own page.
//
// **Home Assistant stores it and does not configure it.** One retained MQTT topic per
// control, keyed by address, so a replacement board recovers the whole map the moment it
// subscribes — the same trick that already recovers the configuration and the rolling
// codes. Deliberately not one document: unlike the configuration, a control map applied
// half-way is harmless, and a document large enough for this house would not fit the
// broker buffer.
//
// It is deliberately **not** persisted to flash. The record store keys records by a byte
// and carries a single u32, so it cannot express an address-keyed entity with a name at
// all; and this map is not needed for the device to do its job. The broker replays it in
// seconds.

namespace ctl {

// Long enough for "Gallery handheld ch3" and short enough that the worst-case payload
// stays well inside the broker buffer.
static const uint8_t NAME_LEN = 24;

// A dozen wall buttons plus two or three multi-channel handhelds, one of them with ten
// channels. Thirty-two at 32 bytes each is 1 KB, which is the cheapest possible insurance
// against running out half way through the walk that fills it.
static const uint8_t MAX_CONTROLS = 32;

// The worst case: the longest name, every index set, and the JSON around them.
static const size_t PAYLOAD_LEN = 192;

struct Control {
  uint32_t address;
  uint32_t drives;   // bitmask over remote indices; rs::MAX_REMOTES is 30, so a u32 fits
  char name[NAME_LEN];
};

// The address comes from the topic rather than the payload, because it is the key. Carried
// twice it could disagree with itself, and the topic is the copy the broker indexes.
//
// `d` is filtered only against the static bound. Whether an index currently exists is a
// question about the configuration document — which arrives *after* these retained topics
// on a cold boot — so answering it here would empty every drives set on exactly the blank
// replacement board this design exists for. The live check happens when a press is applied.
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

  Control parsed = {};
  parsed.address = address;
  strncpy(parsed.name, doc["n"].as<const char *>(), NAME_LEN - 1);

  for (JsonVariantConst index : doc["d"].as<JsonArrayConst>()) {
    const unsigned value = index | 0xFFFFu;
    if (value < rs::MAX_REMOTES) {
      parsed.drives |= (uint32_t)1u << value;
    }
  }

  *out = parsed;
  return true;
}

// Returns 0 rather than truncating: serializeJson() truncates silently, and half a payload
// is a parse failure at the far end instead of a visible fault here.
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

// Every learned control, in RAM. Last write wins per address, because each one is its own
// retained topic and the broker delivers whatever was published last.
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

  // False means the map is full. Refusing is deliberate: silently dropping the control
  // somebody walked across a house to name is the one outcome worth avoiding.
  bool set(const Control &control) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_controls[i].address == control.address) {
        _controls[i] = control;
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

 private:
  Control _controls[MAX_CONTROLS] = {};
  uint8_t _count = 0;
};

}   // namespace ctl
