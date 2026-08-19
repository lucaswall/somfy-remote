# Configuration, and replacing a dead board

The board is the thing most likely to die. Everything it needs to be itself lives in Home
Assistant, so replacing one is a flash and a reboot rather than a rebuild and a walk to
twelve motors.

Two retained MQTT topics carry it:

| Topic | What |
|---|---|
| `<device id>/config` | how many remotes, their addresses, whether each may be driven |
| `<device id>/code/remote<n>` | the rolling counter for each remote |

Both are retained, so the broker replays them to a board the moment it subscribes. The
Mosquitto add-on persists retained messages to disk and that file is inside Home
Assistant's own backups, so they survive a broker restart and a host rebuild.

## The configuration document

```json
{ "v": 1, "epoch": 7, "writer": "ui", "hash": 0,
  "base": "0x000000",
  "remotes": [
    { "i": 0, "enabled": true, "operational": true },
    { "i": 9, "enabled": true, "operational": false }
  ] }
```

- `base` — the address of remote 0. Remote *n* is `base + n` unless it carries its own
  `addr`. **This is the RF credential of the house**; the example above is deliberately
  `0x000000` and `make check` will fail the build if a real one is committed.
- `i` — the index. Indices are never renumbered and never reused: Home Assistant keys its
  entities on them, and the rolling counter belongs to the pair (index, address).
- `enabled` — false removes the remote's entities from Home Assistant but keeps its index
  reserved and its counter intact, so adding it back resumes where it left off.
- `operational` — false means the firmware refuses to transmit for it at all. This is for
  shutters that are known not to work: it is a real guard in the send path, not a note.
- `epoch` — increments on every write. Higher wins. Equal epochs are *not* adopted, so a
  lost update is visible rather than silent.

## Seeding it the first time

`tools/seed_config.py` writes the document from a description of the installation. The
real values belong in `local/`, never in this repository.

```
tools/seed_config.py --host <BROKER_IP> --user <MQTT_USER> --password <MQTT_PASSWORD> \
                     --base 0x000000 --count 12 --not-operational 9,10,11
```

Until the topic exists the device is **unconfigured**: it publishes no entities, refuses
every command, and says so on its own page. That is the correct behaviour for a board that
does not know what it controls — it is not a fault to be worked around.

## Replacing a board

1. Flash the new board over USB. `include/secrets.h` still has to be right: WiFi and MQTT
   credentials cannot come from the network the device has not joined yet.
2. Power it on. It subscribes, waits briefly for the retained topics, adopts the
   configuration and every counter, and comes up.
3. **Check the counters before letting anything drive it.** Compare `/status` against the
   last known-good values. This is the one case the design cannot fully protect: a board
   with no history has no local truth, so it must trust the mirror — and if the broker's
   retained set was *itself* restored from a backup at the same time, the counters can be
   behind the motors. They are then rejected until they catch up.

The counters a board adopts are persisted immediately, not merely believed, so a reboot
with the broker down does not undo the recovery.

## Rolling back to the 2023 firmware

The old firmware reads counters from the fixed EEPROM layout this one no longer writes, so
a rollback needs the counters put back in that shape.

```
tools/rollback_legacy.py --host <BROKER_IP> --user <MQTT_USER> --password <MQTT_PASSWORD> \
                         --out legacy-eeprom.bin
esptool.py -p /dev/cu.usbserial-N write_flash 0x3FA000 erased.bin   # clear both
esptool.py -p /dev/cu.usbserial-N write_flash 0x3FB000 legacy-eeprom.bin
```

Two things that are easy to get wrong:

- **Erase both store sectors**, not just the one being overwritten. If `0x3FA000` keeps a
  valid header, a later re-upgrade finds it, decides migration is already done, and snaps
  every counter back to whatever that stale sector held.
- **A pre-cutover build wipes `0x3FB000` on its first press**, because `EEPROM.commit()`
  erases the whole sector. A reboot alone is harmless; a press is not.

## Names

Display names come from Home Assistant, not from the firmware, and are used only for
display — every internal path is the index. Publish a map to `<device id>/names`:

```json
{ "0": "Office Shutters", "1": "Bedroom Shutters" }
```

`ha/somfy-names.yaml` in this repository is an automation that does it from the entity
registry, so a rename in the Home Assistant UI reaches the device's page on its own. A
remote with no name published simply reads as its index. Nothing breaks if the map never
arrives — which is why it is not cached on the device.
