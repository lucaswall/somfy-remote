#include "store.h"

#include <Arduino.h>

#include "log.h"

// The linker places the emulated-EEPROM sector; the sector below it is the gap between
// the filesystem's end and that, and belongs to nothing.
extern "C" uint32_t _EEPROM_start;

// Sized against a whole-configuration apply, which is the real worst-case burst between
// two loop iterations: 3 scalars plus an ADDR and a FLAGS for every possible remote.
// The press path is not the binding case — Remotes::loop() sends at most one command per
// iteration.
static const uint16_t COMPACT_THRESHOLD = 3 + 2 * rs::MAX_REMOTES;

// The old firmware's layout, read exactly once. Twelve uint16 counters at offsets
// 0,2,...,22 of the EEPROM sector. Never written by this firmware.
static const uint8_t LEGACY_REMOTES = 12;
static const uint16_t LEGACY_COUNT_OFFSET = 60;

uint32_t Store::sectorAddress(uint8_t which) const {
  const uint32_t eeprom = (uint32_t)&_EEPROM_start - 0x40200000u;
  return which == 1 ? eeprom : eeprom - rs::SECTOR_SIZE;
}

bool Store::readSlot(uint8_t which, uint16_t slot, uint8_t *out) const {
  const uint32_t addr = sectorAddress(which) + (uint32_t)slot * rs::RECORD_SIZE;
  return ESP.flashRead(addr, reinterpret_cast<uint32_t *>(out), rs::RECORD_SIZE);
}

bool Store::programSlot(uint8_t which, uint16_t slot, const uint8_t *record) {
  const uint32_t addr = sectorAddress(which) + (uint32_t)slot * rs::RECORD_SIZE;
  if (!ESP.flashWrite(addr, reinterpret_cast<const uint32_t *>(record), rs::RECORD_SIZE)) {
    return false;
  }
  // Read-back, not trust. This is the only thing standing between a worn-out sector and a
  // rolling code that the device believes it stored.
  uint8_t check[rs::RECORD_SIZE];
  if (!readSlot(which, slot, check)) {
    return false;
  }
  return memcmp(check, record, rs::RECORD_SIZE) == 0;
}

bool Store::eraseSector(uint8_t which) {
  return ESP.flashEraseSector(sectorAddress(which) / rs::SECTOR_SIZE);
}

bool Store::writeHeader(uint8_t which, uint32_t generation) {
  uint8_t record[rs::RECORD_SIZE];
  rs::encode(record, rs::NS_HEADER, 0, generation);
  return programSlot(which, 0, record);
}

bool Store::begin() {
  uint8_t slot0[rs::RECORD_SIZE];
  uint32_t genA = 0, genB = 0;
  bool okA = false, okB = false;

  if (readSlot(0, 0, slot0)) {
    okA = rs::sectorHeader(slot0, &genA);
  }
  if (readSlot(1, 0, slot0)) {
    okB = rs::sectorHeader(slot0, &genB);
  }

  const rs::Active active = rs::selectActive(okA, genA, okB, genB);

  if (active == rs::ACTIVE_NONE) {
    // No store anywhere. Either this is the board the 2023 firmware ran on, or it is a
    // replacement that has never been written.
    return migrateLegacy() || bootstrap();
  }

  _active = (active == rs::ACTIVE_B) ? 1 : 0;
  _generation = (active == rs::ACTIVE_B) ? genB : genA;

  // Replay the whole sector a record at a time. Reading 8 bytes at a time rather than
  // buffering 4 KB keeps this off a heap that has ~30 KB free.
  _map.clear();
  _spent = 0;
  uint16_t highestUsed = 0;
  for (uint16_t slot = 1; slot < rs::SLOTS; slot++) {
    uint8_t record[rs::RECORD_SIZE];
    if (!readSlot(_active, slot, record)) {
      continue;
    }
    const rs::SlotState state = rs::classify(record);
    if (state == rs::SLOT_FREE) {
      continue;
    }
    highestUsed = slot;
    if (state == rs::SLOT_SPENT) {
      _spent++;
      continue;
    }
    if (record[0] == rs::NS_HEADER) {
      continue;
    }
    _map.put(record[0], record[1], rs::valueOf(record));
  }
  _appendSlot = (uint16_t)(highestUsed + 1);
  _origin = ORIGIN_REPLAYED;

  logLine("store     : sector %c gen %lu, %u live, %u free, %u spent", activeName(),
          (unsigned long)_generation, _map.count(), freeSlots(), _spent);
  if (_spent > 0) {
    logError("store     : %u torn record(s) skipped on replay", _spent);
  }
  return true;
}

bool Store::migrateLegacy() {
  // The legacy sector is read where the 2023 firmware wrote it. Slot 0 of that sector is
  // counter bytes, not a header, which is why sector state is decided by a valid header
  // rather than by blankness — a "both sectors blank" test could never have fired here.
  uint8_t raw[64];
  if (!ESP.flashRead(sectorAddress(1), reinterpret_cast<uint32_t *>(raw), sizeof(raw))) {
    logError("store     : could not read the legacy region");
    return false;
  }

  bool anyReal = false;
  uint16_t codes[LEGACY_REMOTES];
  for (uint8_t i = 0; i < LEGACY_REMOTES; i++) {
    codes[i] = (uint16_t)((uint16_t)raw[i * 2] | ((uint16_t)raw[i * 2 + 1] << 8));
    if (codes[i] != 0xFFFF) {
      anyReal = true;
    }
  }

  // The sanity gate. An erased sector reads as twelve 0xFFFF words, which is not legacy
  // data — it is a blank board, and guessing counters for one would be far worse than
  // having none. Fall through to bootstrap and let the mirror supply them.
  if (!anyReal) {
    return false;
  }

  // Read for the log only. The new store has no use for a legacy cardinality: how many
  // remotes exist is configuration now, and it comes from Home Assistant.
  const uint16_t legacyCount =
      (uint16_t)((uint16_t)raw[LEGACY_COUNT_OFFSET] |
                 ((uint16_t)raw[LEGACY_COUNT_OFFSET + 1] << 8));

  // An interrupted earlier migration leaves the gap sector non-blank and headerless. Erase
  // it and redo: the source is the untouched legacy sector and the generation is fixed, so
  // the re-run is byte-identical. Without this the device wedges, and the installed board
  // has no USB access without taking it down.
  if (!eraseSector(0)) {
    logError("store     : could not erase the gap sector for migration");
    _origin = ORIGIN_FAILED;
    return false;
  }

  _map.clear();
  for (uint8_t i = 0; i < LEGACY_REMOTES; i++) {
    _map.put(rs::NS_CODE, i, codes[i]);
  }

  _active = 0;
  _generation = 1;
  _appendSlot = 1;
  _spent = 0;

  for (uint8_t i = 0; i < _map.count(); i++) {
    uint8_t record[rs::RECORD_SIZE];
    rs::encode(record, _map.at(i).ns, _map.at(i).id, _map.at(i).value);
    if (!programSlot(0, _appendSlot, record)) {
      logError("store     : migration write failed at slot %u", _appendSlot);
      _origin = ORIGIN_FAILED;
      return false;
    }
    _appendSlot++;
  }

  // Header last: that single write is the commit point, so a power loss anywhere above
  // leaves the legacy sector untouched and this one headerless.
  if (!writeHeader(0, _generation)) {
    logError("store     : migration header write failed");
    _origin = ORIGIN_FAILED;
    return false;
  }

  _origin = ORIGIN_MIGRATED;
  logLine("store     : migrated %u legacy counters (old count field %u) to sector A",
          LEGACY_REMOTES, legacyCount);
  return true;
}

bool Store::bootstrap() {
  if (!eraseSector(0)) {
    logError("store     : could not erase the gap sector");
    _origin = ORIGIN_FAILED;
    return false;
  }
  _map.clear();
  _active = 0;
  _generation = 1;
  _appendSlot = 1;
  _spent = 0;

  // Nothing to protect on a board that never ran the old firmware, so the legacy hold is
  // released immediately. Leaving it set would inhibit compaction forever on a device with
  // no legacy data at all.
  uint8_t record[rs::RECORD_SIZE];
  rs::encode(record, rs::NS_SCALAR, rs::SCALAR_LEGACY_RELEASED, 1);
  if (!programSlot(0, _appendSlot, record)) {
    _origin = ORIGIN_FAILED;
    return false;
  }
  _map.put(rs::NS_SCALAR, rs::SCALAR_LEGACY_RELEASED, 1);
  _appendSlot++;

  if (!writeHeader(0, _generation)) {
    _origin = ORIGIN_FAILED;
    return false;
  }
  _origin = ORIGIN_BOOTSTRAP;
  logLine("store     : blank board, empty store created — waiting for Home Assistant");
  return true;
}

bool Store::put(uint8_t ns, uint8_t id, uint32_t value) {
  if (_origin == ORIGIN_FAILED || _origin == ORIGIN_NONE) {
    return false;
  }

  uint8_t record[rs::RECORD_SIZE];
  rs::encode(record, ns, id, value);

  // Two free slots, not one: the retry below needs somewhere to go.
  for (uint8_t attempt = 0; attempt < 2; attempt++) {
    if (_appendSlot >= rs::SLOTS) {
      logError("store     : sector full and compaction unavailable");
      return false;
    }
    if (programSlot(_active, _appendSlot, record)) {
      _appendSlot++;
      _map.put(ns, id, value);
      return true;
    }
    // The slot is neither free nor valid now, so replay will skip it. There is no
    // tombstone to write — programming can only clear bits.
    logError("store     : slot %u would not take a record, advancing", _appendSlot);
    _appendSlot++;
    _spent++;
  }

  logError("store     : durability failed for ns %u id %u", ns, id);
  return false;
}

void Store::loop() {
  if (_degraded || _origin == ORIGIN_FAILED || _origin == ORIGIN_NONE) {
    return;
  }
  if (freeSlots() > COMPACT_THRESHOLD) {
    return;
  }

  const uint8_t other = (uint8_t)(1 - _active);

  // The gate that makes the legacy release a decision rather than a side effect of
  // housekeeping. Without it the first ordinary compaction — about eleven days after
  // cutover at this installation's press rate — would erase the 2023 counters unasked.
  if (other == 1 && !legacyReleased()) {
    if (freeSlots() == 0) {
      logError("store     : sector full, legacy region not released — appends will fail");
    }
    return;
  }

  if (!compact()) {
    if (!compact()) {
      _degraded = true;
      logError("store     : compaction failed twice, degraded — no further attempts");
    }
  }
}

bool Store::compact() {
  const uint8_t target = (uint8_t)(1 - _active);
  const uint32_t generation = _generation + 1;

  if (!eraseSector(target)) {
    logError("store     : erase of sector %c failed", target == 0 ? 'A' : 'B');
    return false;
  }

  uint16_t slot = 1;
  for (uint8_t i = 0; i < _map.count(); i++) {
    if (slot >= rs::SLOTS) {
      logError("store     : live set does not fit a sector");
      return false;
    }
    uint8_t record[rs::RECORD_SIZE];
    rs::encode(record, _map.at(i).ns, _map.at(i).id, _map.at(i).value);
    if (!programSlot(target, slot, record)) {
      logError("store     : compaction write failed at slot %u", slot);
      return false;
    }
    slot++;
  }

  // Until this lands, the old sector is still the active one. A power loss above leaves
  // the target headerless and therefore foreign, and replay falls back cleanly.
  if (!writeHeader(target, generation)) {
    logError("store     : compaction header write failed");
    return false;
  }

  _active = target;
  _generation = generation;
  _appendSlot = slot;
  _spent = 0;
  logLine("store     : compacted to sector %c gen %lu, %u live, %u free", activeName(),
          (unsigned long)_generation, _map.count(), freeSlots());
  return true;
}

bool Store::releaseLegacy() {
  if (legacyReleased()) {
    return true;
  }
  if (_origin != ORIGIN_MIGRATED && _origin != ORIGIN_REPLAYED) {
    return false;
  }
  if (!put(rs::NS_SCALAR, rs::SCALAR_LEGACY_RELEASED, 1)) {
    return false;
  }
  if (!eraseSector(1)) {
    logError("store     : legacy region erase failed");
    return false;
  }
  logLine("store     : legacy region released and erased");
  return true;
}
