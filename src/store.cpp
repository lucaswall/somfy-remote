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
    return bootstrap();
  }

  _active = (active == rs::ACTIVE_B) ? 1 : 0;
  _generation = (active == rs::ACTIVE_B) ? genB : genA;

  // Reading 8 bytes at a time rather than buffering a whole sector keeps replay off a
  // heap with about 20 KB free.
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

