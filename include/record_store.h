#pragma once

#include <stdint.h>
#include <string.h>

// The append-only record store: the whole persistence format, as pure logic over a
// caller-supplied byte array. No flash calls, no Arduino headers, so `make test` can
// exercise every crash path that hardware cannot be made to reproduce on demand.
//
// **Why a log and not a record.** The Arduino EEPROM library erases and rewrites a whole
// 4 KB sector on every commit, so changing two bytes costs one erase/program cycle of the
// sector's rated life. The counters recovered from the board this firmware replaces sum
// to ~47,880 presses, which is ~48% of a typical 100,000-cycle rating — on a flash part
// whose vendor id (0xD8) matches no mainstream manufacturer and which therefore has no
// published rating at all.
//
// NOR flash programs by clearing bits and erases by setting them, and only erase is
// sector-wide. So appending an 8-byte record into already-erased space costs no erase at
// all, and one erase now covers a sector's worth of presses instead of one. That is also
// what takes the erase out of the send path: a program is microseconds, a sector erase is
// tens of milliseconds, and today one of those sits between the button press and the
// frame leaving the antenna.
//
// **Why generic key/value and not a rolling-code journal.** The namespace byte is one
// byte of a record that has to exist anyway. Without it, the first thing that needs
// persisting after the counters forces a second format migration; with it, it takes the
// next free namespace id.

namespace rs {

// 4096-byte sector, 8-byte records. Eight is a multiple of four, which ESP.flashWrite()
// requires, and every slot is therefore 8-byte aligned.
static const uint16_t RECORD_SIZE = 8;
static const uint16_t SECTOR_SIZE = 4096;
static const uint16_t SLOTS = SECTOR_SIZE / RECORD_SIZE;   // 512

// Namespaces. Adding a future data type means taking the next free id; the format does
// not change and no second migration is needed.
static const uint8_t NS_CODE = 0x00;     // id = remote index, value = monotonic counter
static const uint8_t NS_SCALAR = 0x01;   // id = SCALAR_*, value below
static const uint8_t NS_ADDR = 0x02;     // id = remote index, value = RF address
static const uint8_t NS_FLAGS = 0x03;    // id = remote index, value = bitfield
static const uint8_t NS_HEADER = 0xFE;   // slot 0 only, value = sector generation
static const uint8_t NS_FREE = 0xFF;     // not a namespace: an erased slot

static const uint8_t SCALAR_REMOTE_COUNT = 0;      // highest configured index + 1
static const uint8_t SCALAR_ADDRESS_BASE = 1;
static const uint8_t SCALAR_CONFIG_EPOCH = 2;
static const uint8_t SCALAR_LEGACY_RELEASED = 3;   // until set, the legacy sector is not erased

// No override. ADDR cannot express a delete — programming only clears bits — so a config
// that stops naming a remote writes this instead, and the address falls back to
// base + index. Without it a removed remote's override would survive compaction forever
// and be silently inherited by whatever later took the index.
static const uint32_t ADDR_NONE = 0xFFFFFFFFu;

static const uint32_t FLAG_ENABLED = 1u << 0;
static const uint32_t FLAG_OPERATIONAL = 1u << 1;

// 30 remotes x 3 per-remote namespaces + 4 scalars = 94 live records at the maximum
// configuration, plus a little headroom for namespaces a newer firmware may have written
// and this one must carry forward rather than destroy.
static const uint8_t MAX_REMOTES = 30;
static const uint8_t MAX_ENTRIES = 104;

// CRC-8, polynomial 0x07, init 0xFF, no reflection, no final xor.
//
// The init value is load-bearing and must not be changed to 0x00. A slot that failed to
// program can read back as all zeros, and with a zero init an all-zero record computes a
// CRC of 0x00 — so the failed slot would read back as a *valid* CODE[0] = 0 record and
// last-write-wins would apply it, silently resetting a counter to zero.
inline uint8_t crc8(const uint8_t *data, uint8_t len) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      crc = (uint8_t)((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
    }
  }
  return crc;
}

inline void encode(uint8_t *out, uint8_t ns, uint8_t id, uint32_t value) {
  out[0] = ns;
  out[1] = id;
  out[2] = (uint8_t)(value & 0xFF);
  out[3] = (uint8_t)((value >> 8) & 0xFF);
  out[4] = (uint8_t)((value >> 16) & 0xFF);
  out[5] = (uint8_t)((value >> 24) & 0xFF);
  out[6] = 0x00;   // flags: reserved, and part of the validity predicate
  out[7] = crc8(out, 7);
}

inline uint32_t valueOf(const uint8_t *slot) {
  return (uint32_t)slot[2] | ((uint32_t)slot[3] << 8) | ((uint32_t)slot[4] << 16) |
         ((uint32_t)slot[5] << 24);
}

enum SlotState {
  SLOT_FREE,    // all 8 bytes erased — the tail of the log
  SLOT_VALID,   // passes the validity predicate
  SLOT_SPENT,   // neither: a torn program, or a record from a corrupted write
};

// The validity predicate, in one place. A slot is free only if every byte is erased;
// valid only if the namespace is real, the reserved flags byte is clear, and the CRC
// matches. Everything else is spent and is skipped exactly like a CRC failure.
//
// There is deliberately no tombstone and no "mark this slot dead" operation: after a
// failed program every bit of `flags` is already clear, so no bit remains to raise, and
// programming the slot to all zeros would produce the valid-looking CODE[0] = 0 that the
// CRC init above exists to prevent.
inline SlotState classify(const uint8_t *slot) {
  bool allFree = true;
  for (uint8_t i = 0; i < RECORD_SIZE; i++) {
    if (slot[i] != 0xFF) {
      allFree = false;
      break;
    }
  }
  if (allFree) {
    return SLOT_FREE;
  }
  if (slot[0] == NS_FREE || slot[6] != 0x00 || slot[7] != crc8(slot, 7)) {
    return SLOT_SPENT;
  }
  return SLOT_VALID;
}

struct Entry {
  uint8_t ns;
  uint8_t id;
  uint32_t value;
};

// The live set: the last written value for every (ns, id). Bounded by MAX_ENTRIES, which
// is what bounds the RAM cost of replay.
class LiveMap {
 public:
  void clear() { _count = 0; }

  bool put(uint8_t ns, uint8_t id, uint32_t value) {
    for (uint8_t i = 0; i < _count; i++) {
      if (_entries[i].ns == ns && _entries[i].id == id) {
        _entries[i].value = value;
        return true;
      }
    }
    if (_count >= MAX_ENTRIES) {
      return false;
    }
    _entries[_count++] = {ns, id, value};
    return true;
  }

  bool get(uint8_t ns, uint8_t id, uint32_t *out) const {
    for (uint8_t i = 0; i < _count; i++) {
      if (_entries[i].ns == ns && _entries[i].id == id) {
        *out = _entries[i].value;
        return true;
      }
    }
    return false;
  }

  bool has(uint8_t ns, uint8_t id) const {
    uint32_t ignored;
    return get(ns, id, &ignored);
  }

  uint32_t valueOr(uint8_t ns, uint8_t id, uint32_t fallback) const {
    uint32_t out;
    return get(ns, id, &out) ? out : fallback;
  }

  uint8_t count() const { return _count; }
  const Entry &at(uint8_t i) const { return _entries[i]; }

 private:
  Entry _entries[MAX_ENTRIES] = {};
  uint8_t _count = 0;
};

// --- sector level ---------------------------------------------------------------------

// A sector's state is decided by slot 0 alone. "Foreign" covers blank, half-written and
// legacy content alike — the installed board's legacy sector holds counter bytes in slot
// 0, so a test for "blank" would never have fired on the only device needing migration.
inline bool sectorHeader(const uint8_t *sector, uint32_t *generation) {
  if (classify(sector) != SLOT_VALID || sector[0] != NS_HEADER || sector[1] != 0) {
    return false;
  }
  *generation = valueOf(sector);
  return true;
}

enum Active { ACTIVE_NONE, ACTIVE_A, ACTIVE_B };

inline Active selectActive(bool aValid, uint32_t aGen, bool bValid, uint32_t bGen) {
  if (aValid && bValid) {
    return (bGen > aGen) ? ACTIVE_B : ACTIVE_A;
  }
  if (aValid) {
    return ACTIVE_A;
  }
  if (bValid) {
    return ACTIVE_B;
  }
  return ACTIVE_NONE;
}

struct ReplayResult {
  uint16_t appendSlot;   // first slot a new record may be programmed into
  uint16_t spent;        // slots skipped as torn or corrupt, surfaced as faults
  bool overflow;         // the live set exceeded MAX_ENTRIES
};

// Replays a whole sector into the map. The entire sector is scanned rather than stopping
// at the first free slot, so a torn record cannot hide valid data written after it. The
// append pointer is one past the highest *non-free* slot, not the first free one — those
// differ exactly when a torn write sits before free space, and using the wrong one would
// re-program an occupied slot.
inline ReplayResult replay(const uint8_t *sector, LiveMap *map) {
  ReplayResult r = {1, 0, false};
  uint16_t highestUsed = 0;
  for (uint16_t slot = 1; slot < SLOTS; slot++) {
    const uint8_t *p = sector + (uint32_t)slot * RECORD_SIZE;
    const SlotState state = classify(p);
    if (state == SLOT_FREE) {
      continue;
    }
    highestUsed = slot;
    if (state == SLOT_SPENT) {
      r.spent++;
      continue;
    }
    if (p[0] == NS_HEADER) {
      continue;   // a header anywhere but slot 0 is not a live value
    }
    if (!map->put(p[0], p[1], valueOf(p))) {
      r.overflow = true;
    }
  }
  r.appendSlot = (uint16_t)(highestUsed + 1);
  return r;
}

// Serialises the live set for compaction, into slots 1..n. The header is deliberately not
// included: it is programmed last and alone, and that single write is the commit point.
// Returns the number of records written.
inline uint16_t snapshotBody(const LiveMap &map, uint8_t *out, uint16_t maxRecords) {
  uint16_t n = 0;
  for (uint8_t i = 0; i < map.count() && n < maxRecords; i++) {
    encode(out + (uint32_t)n * RECORD_SIZE, map.at(i).ns, map.at(i).id, map.at(i).value);
    n++;
  }
  return n;
}

// --- counters -------------------------------------------------------------------------

// The stored counter is a monotonic u32; the transmitted code is its low 16 bits.
//
// This is what makes "a counter never moves backwards" expressible. Somfy's code is 16
// bits and wraps, and max() over a wrapping quantity is not a total order —
// max(0x0000, 0xFFFF) would re-select a code that has already been transmitted. Keeping
// the counter monotonic puts the wrap where it belongs, in the frame codec, and costs
// nothing because the value field is already four bytes wide.
inline uint16_t transmitCode(uint32_t counter) { return (uint16_t)(counter & 0xFFFF); }

enum Reconcile {
  REC_NONE,             // neither side has a counter: the remote cannot transmit
  REC_KEEP_QUIET,       // local stands, the mirror has nothing to correct
  REC_KEEP_PUBLISH,     // local stands and the mirror is behind: publish upward
  REC_ADOPT,            // the mirror is ahead: adopt it *and persist it*
  REC_REFUSE_JUMP,      // the mirror is implausibly far ahead: refuse and surface
};

// The one rule the whole design rests on: the mirror is a floor in both directions.
// Home Assistant may raise a counter on a device that is behind; a device may never
// lower one, and never publishes at all when the mirror is merely absent.
//
// The absent case is not a detail. A blank replacement board has local = 0, and treating
// "absent" as "we won" would publish 0 over a retained 914 and destroy the only durable
// copy of the counter — the recovery path eating the thing it recovers from.
inline Reconcile reconcile(bool haveLocal, uint32_t local, bool haveMirror, uint32_t mirror,
                           uint32_t maxJump, uint32_t *effective) {
  if (!haveLocal && !haveMirror) {
    return REC_NONE;
  }
  if (!haveMirror) {
    *effective = local;
    return REC_KEEP_QUIET;
  }
  if (!haveLocal) {
    // No local baseline to measure a jump against; the mirror is all there is.
    *effective = mirror;
    return REC_ADOPT;
  }
  if (mirror > local) {
    if (mirror - local > maxJump) {
      *effective = local;
      return REC_REFUSE_JUMP;
    }
    *effective = mirror;
    return REC_ADOPT;
  }
  *effective = local;
  return (mirror < local) ? REC_KEEP_PUBLISH : REC_KEEP_QUIET;
}

}   // namespace rs
