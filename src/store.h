#pragma once

#include <stdint.h>

#include "record_store.h"

// The record store bound to real flash: two sectors, append-only, compacted in the main
// loop. All of the format logic lives in include/record_store.h and is tested on the
// desktop; this file is only the part that cannot be — erase, program, and read-back.
//
// **The two sectors.** The linker gives `_EEPROM_start` (flash offset 0x3FB000 on this
// board) and `_FS_end` one sector below it (0x3FA000). That gap sector belongs to nothing
// — not the filesystem, not the SDK's RF calibration region — and it was verified empty
// on the installed board. Deriving both from the linker symbol rather than hardcoding
// them means a change of filesystem size moves them together, which is exactly what the
// 4m1m and 4m2m layouts do.
class Store {
 public:
  // Result of bringing the store up, so the boot banner can say what happened rather than
  // leaving a migration to be inferred from counter values.
  enum Origin {
    ORIGIN_NONE,        // nothing yet: waiting on the mirror
    ORIGIN_REPLAYED,    // an existing store was found
    ORIGIN_MIGRATED,    // legacy counters were carried across
    ORIGIN_BOOTSTRAP,   // blank board, empty store created
    ORIGIN_FAILED,      // migration or bootstrap could not be verified
  };

  bool begin();

  const rs::LiveMap &map() const { return _map; }
  Origin origin() const { return _origin; }

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
  bool legacyReleased() const {
    return _map.valueOr(rs::NS_SCALAR, rs::SCALAR_LEGACY_RELEASED, 0) != 0;
  }
  // 'A' for the gap sector, 'B' for the one the old firmware used as EEPROM.
  char activeName() const { return _active == 0 ? 'A' : 'B'; }

  // The legacy region is erased only once its contents are provably redundant. Until then
  // compaction refuses to touch it, so the device runs single-sector rather than quietly
  // performing the irreversible step behind the operator's back.
  bool releaseLegacy();

  // True while the legacy sector still holds the 2023 counters and compaction is inhibited.
  bool legacyHeld() const { return !legacyReleased() && _origin != ORIGIN_NONE; }

 private:
  uint32_t sectorAddress(uint8_t which) const;
  bool readSlot(uint8_t which, uint16_t slot, uint8_t *out) const;
  bool programSlot(uint8_t which, uint16_t slot, const uint8_t *record);
  bool eraseSector(uint8_t which);
  bool compact();
  bool migrateLegacy();
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
