# Rolling code storage

The counters cannot be regenerated. Lose one and that motor has to be paired again by
hand. This is why the storage looks the way it does; the format itself is in
`include/record_store.h`, which is the authority.

## Why a log instead of a record

Rewriting a value in place costs a **4 KB sector erase**, because that is the only
granularity NOR flash erases at. The Arduino `EEPROM` library, the obvious way to do this,
is exactly that: `commit()` erases the sector and rewrites all of it. Changing two bytes on
every press would spend one erase/program cycle of that sector's rated life each time.

Programming is different: it only clears bits, and works on any 4-byte-aligned word. So
appending an 8-byte record into already-erased space costs no erase at all. One erase then
covers a sector's worth of presses rather than one, and two sectors in rotation halve it
again — a few dozen erases a year instead of thousands.

It also keeps the erase off the send path. A program is microseconds; a sector erase is
tens of milliseconds, and that would otherwise sit between the button press and the frame
leaving the antenna.

## The two sectors

Flash offsets `0x3FA000` and `0x3FB000` on a 4 MB board: the sector the linker calls
`_EEPROM_start`, and the unclaimed one below it between there and `_FS_end`. Neither
belongs to the filesystem or the SDK's calibration region, and both are identical across
the 4m1m and 4m2m layouts. They are derived from the linker symbol rather than hardcoded,
so changing the filesystem size moves them together.

Sector state is decided by **slot 0 alone** — a valid header record, or nothing. Anything
else (blank, half-written, foreign) is not a header, and the other sector wins.

## The commit point

Compaction writes the live set into the inactive sector, verifies it, and only then writes
slot 0's header. That single 8-byte program is the commit: a power loss anywhere before it
leaves the target headerless, so the old sector is still active and no data is at risk.
At no instant is there no valid sector.

Compaction runs from the main loop on a fill threshold, never from the press path, and a
compaction that fails twice latches a degraded flag rather than erase-looping on a failing
sector.

## The counter is a monotonic u32

Somfy's code is 16 bits and wraps; the stored counter does not. It only increases, and the
transmitted code is its low 16 bits.

This is what makes "never move backwards" expressible at all. Over a wrapping `u16`,
`max()` is not a total order — `max(0x0000, 0xFFFF)` would re-select a code that has
already been sent. Keeping the counter monotonic puts the wrap where it belongs, in the
frame codec. It costs nothing: the value field is already four bytes.

## How it fails

| Event | Outcome |
|---|---|
| Power loss mid-append | The record is spent and skipped; the previous value stands. The append happens *before* the transmit, so nothing reached the air and the next press sends that code for the first time. Cost: zero. |
| Power loss mid-compaction | Target is headerless, so the old sector is still active. |
| A write that will not verify | Retried once into the next slot, then reported — and the frame is **not** transmitted. Not moving is always safe. |
| Sector worn out | The above, surfaced on `/errors` and as a Home Assistant diagnostic rather than failing silently. |
| Older firmware after newer | Unknown namespaces are carried through compaction rather than dropped. |

## Extending it

A new kind of persisted value takes the next free namespace id. The format does not
change and nothing has to be migrated — which is the reason this is a generic key/value
log rather than a counter journal.
