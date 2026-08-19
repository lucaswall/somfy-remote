# How rolling codes are stored

The counters are the one thing here that cannot be regenerated. Lose them and every motor
has to be paired again by hand. This is the format that holds them, and the reasoning
behind each part of it.

## Why not just write them down

The firmware this replaces kept twelve `uint16` counters at fixed offsets in the emulated
EEPROM sector, and called `EEPROM.commit()` on every press. That call is not a two-byte
write:

```
ESP.flashEraseSector(sector)   // all 4096 bytes
ESP.flashWrite(...)            // all of them back
```

So every press cost one erase/program cycle of that sector's rated life. The counters
recovered from the installed board summed to about 47,880 presses — roughly **half** a
typical 100,000-cycle NOR rating, on a part whose JEDEC manufacturer id (`0xD8`) matches
no mainstream vendor and therefore has no published rating at all. Two remotes accounted
for 90% of it.

Flash does not fail loudly. It fails as a write that silently does not stick, which
presents as a shutter that has stopped responding.

## The shape of the fix

NOR flash programs by clearing bits and erases by setting them, and only erase works on a
whole sector. Programming into space that is *already* erased therefore costs no erase at
all. So: stop overwriting a record, start appending to a log.

A press writes one 8-byte record into the next free slot. A 4 KB sector holds 512 of them,
so one erase now covers hundreds of presses instead of one. Two sectors in rotation halve
it again. At this installation's rate that is roughly 17 erases per sector per year —
about 5,800 years at 100,000 cycles, and about 580 even if the part turns out to be a
tenth as durable as assumed.

It also takes the erase off the press path. A program is microseconds; a sector erase is
tens of milliseconds, and one used to sit between the button press and the frame leaving
the antenna.

## The record

```
offset  size  field
0       1     ns      namespace; 0xFF = free slot, 0xFE = sector header
1       1     id      key within the namespace
2       4     value   little-endian u32
6       1     flags   reserved, always 0x00
7       1     crc     CRC-8 over offsets 0..6
```

Eight bytes is a multiple of four, which `ESP.flashWrite()` requires, and every slot is
8-byte aligned.

**CRC-8: polynomial `0x07`, init `0xFF`, no reflection, no final XOR.** The init value is
load-bearing. A slot that failed to program can read back as all zeros, and with a zero
init an all-zero record computes a CRC of `0x00` — so the dead slot would pass as a valid
`CODE[0] = 0` record and reset a counter to zero. With `0xFF` it fails closed.

A slot is **free** only if all eight bytes are erased, **valid** if the namespace is real,
`flags` is clear and the CRC matches, and **spent** otherwise. Spent slots are skipped on
replay exactly like a CRC failure.

There is no tombstone and no "mark this slot dead" operation. After a failed program every
bit of `flags` is already clear, so no bit remains to raise — and writing all zeros would
produce exactly the false-valid record the CRC init exists to prevent.

## Namespaces

| ns | meaning |
|---|---|
| `0x00` | rolling counter, keyed by remote index |
| `0x01` | scalars — remote count, address base, config epoch, legacy-released |
| `0x02` | RF address override, `0xFFFFFFFF` meaning "derive from base + index" |
| `0x03` | flags — enabled, operational |
| `0xFE` | sector header, slot 0 only, value = generation |

Anything new takes the next free namespace id. The format does not change and no second
migration is needed — which is the whole reason this is a key/value log rather than a
purpose-built counter journal.

**Records in namespaces this firmware does not recognise are carried forward through
compaction**, so running an older build after a newer one does not destroy what the newer
one wrote.

## The counter is a monotonic u32

Somfy's code is 16 bits and wraps. The stored counter is not: it is a `u32` that only ever
increases, and the transmitted code is its low 16 bits.

This is what makes "never move backwards" expressible at all. Over a wrapping `u16`,
`max()` is not a total order — `max(0x0000, 0xFFFF)` would re-select a code that has
already been sent. Keeping the counter monotonic puts the wrap where it belongs, in the
frame codec. It costs nothing: the value field is already four bytes.

## Sectors and the commit point

Two sectors: flash offsets `0x3FA000` and `0x3FB000` on a 4 MB board. The second is the
one the linker calls `_EEPROM_start`; the first is the unclaimed sector between it and
`_FS_end`, which belongs to neither the filesystem nor the SDK's calibration region. Both
addresses are identical across the 4m1m and 4m2m layouts.

A sector's state is decided by **slot 0 alone**: a valid header record or nothing. Three
states — active (valid header, higher generation), inactive (valid header, lower), and
foreign (no valid header, covering blank, half-written and legacy content alike).

Compaction, when the free slots run low:

1. Erase the inactive sector.
2. Write the live set into slots 1..n.
3. Read back and verify every record.
4. Write slot 0's header with generation + 1.

**Step 4 is the commit point** — one 8-byte program, written last and alone. A power loss
anywhere above it leaves the target headerless, so it reads as foreign and the old sector
still wins. At no instant is there no valid sector.

Compaction runs from the main loop, never from the press path, and it verifies; a
compaction that fails twice latches a degraded flag rather than retrying forever on a
sector that is failing.

## What happens when things go wrong

| Event | Outcome |
|---|---|
| Power loss mid-append | The record is spent and skipped; the previous value stands. The append happens *before* the transmit, so nothing reached the air and the next press sends that code for the first time. Cost: zero. |
| Power loss mid-compaction | Target is headerless and therefore foreign; the old sector is still active. |
| A write that will not verify | Retried once into the next slot, then reported as a durability failure — and the frame is **not** transmitted. Not moving is always safe. |
| Sector worn out | The above, surfaced on `/errors` instead of failing silently as the old scheme did. |
| Older firmware run after newer | Unknown namespaces survive compaction. |

## Migration from the old layout

Runs once, on the first boot of this firmware, before any network state is considered.

The legacy sector's slot 0 holds counter bytes, not a header, so it reads as *foreign* —
which is why sector state is defined by a valid header rather than by blankness. A check
for "blank" would never have fired on the one board that needed migrating.

The twelve `uint16` counters are read from their old offsets and written as the first
snapshot into `0x3FA000`, header last. Twelve `0xFFFF` words mean an erased sector rather
than legacy data — that is a blank board, and it falls through to waiting for the mirror
instead of inventing counters.

The old region is then **held, not erased**. Compaction refuses to touch it until a
`LEGACY_RELEASED` scalar is set, and that is set only once every migrated counter is
provably also present in the MQTT mirror at the same value. Until then the device runs on
one sector. This is deliberate: without the hold, ordinary housekeeping would have erased
the 2023 counters about eleven days after cutover, unasked.
