# Configuration, and replacing a board

The board is the thing most likely to die. Everything it needs to be itself lives in Home
Assistant, so replacing one is a flash and a reboot rather than a rebuild and a walk to
every motor.

Three retained MQTT topics carry it:

| Topic | What |
|---|---|
| `<device id>/config` | how many remotes, their addresses, whether each may be driven |
| `<device id>/code/remote<n>` | the rolling counter for each remote |
| `<device id>/names` | display names, published by Home Assistant |

Retained, so the broker replays them to a board the moment it subscribes. Mosquitto
persists retained messages to disk and that file is inside Home Assistant's backups, so
they survive a broker restart and a host rebuild.

## The configuration document

```json
{ "v": 1, "epoch": 7, "writer": "ui", "hash": 0,
  "base": "0x000000",
  "remotes": [
    { "i": 0, "enabled": true, "operational": true },
    { "i": 9, "enabled": true, "operational": false }
  ] }
```

- `base` — the address of remote 0; remote *n* is `base + n` unless it carries its own
  `addr`. **This is the RF credential of the installation.** The example is deliberately
  `0x000000`, and `make check` fails the build if a real one is committed.
- `i` — the index. Indices are never renumbered and never reused: Home Assistant keys its
  entities on them, and a counter is only meaningful for the address it was issued against.
- `enabled` — false removes the remote's entities from Home Assistant, keeping its index
  reserved and its counter intact.
- `operational` — false means the firmware refuses to transmit for it at all. For a shutter
  known not to work, so that "do not drive this one" is enforced rather than remembered.
- `epoch` — increments on every write; higher wins. Equal epochs are *not* adopted, so a
  lost update is visible rather than silent.

## Seeding it

`tools/seed_config.py` writes the document. Real values belong in `local/`, never here.

```
tools/seed_config.py --host <BROKER_IP> --user <MQTT_USER> --password <MQTT_PASSWORD> \
                     --base 0x000000 --count 8 --not-operational 6,7
```

Until the topic exists the device is **unconfigured**: no entities, every command refused,
and it says so on its own page. That is correct for a board that does not know what it
controls, not a fault to work around.

## Replacing a board

1. Flash over USB. `include/secrets.h` still has to be right — WiFi and MQTT credentials
   cannot come from a network the device has not joined.
2. Power it on. It subscribes, waits briefly for the retained topics, adopts the
   configuration and every counter, and comes up.
3. **Check the counters before letting anything drive it.** A board with no history has no
   local truth, so it must trust the mirror — and if the broker's retained set was itself
   restored from a backup at the same time, the counters can be behind the motors and
   commands are rejected until they catch up.

Adopted counters are persisted before they are believed, so a reboot with the broker down
does not undo the recovery.

## Names

Display names come from Home Assistant and are used only for display — every internal path
is the index. `ha/somfy-names.yaml` publishes the map from the entity registry, so a rename
in the Home Assistant UI reaches the device's page on its own.

A remote with no name published reads as its index. Nothing breaks if the map never
arrives, which is why it is not cached on the device.
