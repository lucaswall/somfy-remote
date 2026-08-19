#pragma once

#include <stdint.h>

#include "record_store.h"

// The record store bound to real flash: two sectors, append-only, compacted in the main
// loop. All of the format logic lives in include/record_store.h and is tested on the
// desktop; this file is only the part that cannot be — erase, program, and read-back.
//
// **The two sectors.** The linker gives `_EEPROM_start`, and `_FS_end` is one sector below
// it. That gap sector belongs to nothing — not the filesystem, not the SDK's RF
// calibration region. Deriving both from the linker symbol rather than hardcoding them
// means a change of filesystem size moves them together, which is what the 4m1m and 4m2m
// layouts do.
class Store {
 public:
  enum Origin {
    ORIGIN_NONE,        // nothing yet: waiting on the mirror
    ORIGIN_REPLAYED,    // an existing store was found
    ORIGIN_BOOTSTRAP,   // blank board, empty store created
    ORIGIN_FAILED,      // could not be verified
  };

  bool begin();

  const rs::LiveMap &map() const { return _map; }

  // Appends a record and verifies it by read-back before returning. False means the value
  // is not durable, and the caller must not act as though it were — for a rolling code
  // that means not transmitting.
  bool put(uint8_t ns, uint8_t id, uint32_t value);

  // Compaction lives here, not on the press path: it is the only remaining erase, and an
  // erase is tens of milliseconds.
  void loop();

  uint32_t valueOr(uint8_t ns, uint8_t id, uint32_t fallback) const {
    return _map.valueOr(ns, id, fallback);
  }
  bool has(uint8_t ns, uint8_t id) const { return _map.has(ns, id); }

  uint16_t freeSlots() const { return (uint16_t)(rs::SLOTS - _appendSlot); }
  uint16_t spent() const { return _spent; }
  uint32_t generation() const { return _generation; }
  bool degraded() const { return _degraded; }
  char activeName() const { return _active == 0 ? 'A' : 'B'; }

 private:
  uint32_t sectorAddress(uint8_t which) const;
  bool readSlot(uint8_t which, uint16_t slot, uint8_t *out) const;
  bool programSlot(uint8_t which, uint16_t slot, const uint8_t *record);
  bool eraseSector(uint8_t which);
  bool compact();
  bool bootstrap();
  bool writeHeader(uint8_t which, uint32_t generation);

  rs::LiveMap _map;
  uint8_t _active = 0;
  uint16_t _appendSlot = 1;
  uint16_t _spent = 0;
  uint32_t _generation = 0;
  bool _degraded = false;
  Origin _origin = ORIGIN_NONE;
};
