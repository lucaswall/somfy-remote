# somfy-remote

An ESP8266 + CC1101 bridge that puts Somfy RTS roller shutters and curtains onto Home
Assistant over MQTT.

Somfy RTS motors have no smart interface of any kind. They are driven by handheld remotes
over one-way 433.42 MHz radio, and there is nothing for Home Assistant to talk to. This
bridge is not a gateway in front of the motors — it is **another handful of remotes**,
each with its own address and its own rolling code counter, transmitting the same protocol
the physical ones do:

```
handheld remote ──433.42 MHz RTS──┐
                                  ├──> shutter motor
   this bridge ───────────────────┘
        │
        └── MQTT discovery ──> Home Assistant
```

Both keep working, and neither is aware of the other.

Everything except the radio runs on a Wemos D1 mini: WiFi, OTA, a debug web UI at
`http://somfy-remote.local/`, and the MQTT integration.

## What it exposes

One Home Assistant device per emulated remote, three entities each:

- **Cover** — Up, Down and Stop, with Stop wired to the remote's My button
- **My** — a switch rather than a button, because Google Home does not surface buttons
  usefully. It reports itself back off after each press
- **Rolling code** — a diagnostic sensor. A counter that stops advancing while presses are
  still being logged is what a flash that has stopped accepting writes looks like from
  outside, and that is worth an alert rather than a glance at a web page

Plus a `Somfy Bridge` device carrying store diagnostics, and a retained availability topic
backed by an MQTT last will, so a crashed bridge shows as unavailable rather than as
shutters that have quietly stopped responding.

**Pairing is deliberately not here.** Home Assistant mirrors what the bridge *does* — open,
stop, close — not how it is configured, and enrolling a remote at a motor is configuration.
Prog lives on the device's own settings page, behind a password, next to the address it
pairs. A one-tap unconfirmed button on a dashboard is the wrong home for the one action
that cannot be undone by pressing something else.

### What it cannot know

RTS is **one-way**. The motor never reports anything — not its position, not an
acknowledgement, not its presence. Everything Home Assistant shows is inferred from what
the bridge sent:

- After Up the cover reports open, after Down it reports closed. After a Stop it reports
  whichever it last was, because the protocol cannot say where it stopped.
- If somebody uses the original handheld remote, the bridge has no idea.
- A command lost to interference leaves Home Assistant optimistic and wrong until the next
  one. Five repeats per press help; certainty is not available.

The entities are declared assumed-state for exactly this reason: Home Assistant shows both
buttons at all times rather than hiding the one it believes is redundant.

## Hardware

| Part | Notes |
|---|---|
| Wemos D1 mini (ESP8266) | PlatformIO board id `d1_mini`. Any ESP8266 works; the pin map here is mini-specific |
| CC1101 433 MHz module | E07-M1101D or equivalent. 3.3 V only. Wiring in [`docs/hardware.md`](docs/hardware.md) |
| 7 dupont wires | GDO0, CSN, SCK, MOSI, MISO, VCC, GND. GDO2 unused |
| A 433 MHz antenna | Fit it before powering up |

## Quick start

```bash
# macOS, Apple Silicon: the xtensa compiler is x86_64-only
softwareupdate --install-rosetta --agree-to-license
brew install platformio

cp include/secrets.h.example include/secrets.h   # then fill it in
make test          # desktop unit tests, no board needed
make radio         # prove the CC1101 is wired, before anything else
make run           # build, flash, print the boot banner
```

`secrets.h` carries the WiFi and MQTT credentials, the OTA password and the login for the
web UI's settings page. It deliberately does **not** carry the remotes'
addresses or how many there are: that is configuration, and it lives in a retained MQTT
document so a replacement board can recover it rather than needing a rebuild. See
[`docs/configuration.md`](docs/configuration.md) for seeding it the first time.

**Choose the address base once and write it down.** A motor is paired to an address;
changing it means walking to every shutter and pairing it again. If you are replacing an
earlier firmware on an installed board, read the last section of
[`docs/hardware.md`](docs/hardware.md) first — there are two values on that board that
must survive, and neither of them is in this repository.

Once a board is running this firmware, reflash it over the air with
`make ota OTA_HOST=somfy-remote.local`. That only works while the *running* sketch handles
OTA, so one bad flash puts you back on USB.

`make help` lists every target.

## Pairing a shutter

1. Hold Prog on a remote the motor already knows until the shutter jogs.
2. Press the Prog button of the remote you want to add, in Home Assistant or on the web
   page, within a couple of seconds. The shutter jogs again.

Sending Prog to a motor that already knows that address unpairs it, which is why the web
UI asks first.

## HTTP endpoints

Everything the board serves, on port 80.

Two pages. `/` is operation and needs no password; `/settings` is administration and asks
for one. Everything under `/settings` is behind the same login.

| Endpoint | Purpose |
|---|---|
| `GET /` | Operation: per-remote Up/My/Down, board status, live console |
| `GET /api/state` | State as JSON, plus IP, RSSI, uptime, heap and queue depth. Polled by both pages |
| `POST /api/send` | `?remote=<n>&command=Up\|My\|Down`. Refuses `Prog` |
| `GET /status` | Snapshot: build stamp, reset reason, uptime, heap, WiFi, store, and a line per remote |
| `GET /log` | The console ring as plain text, oldest first |
| `GET /errors` | Faults only, from a separate smaller ring |
| `GET /settings` | Administration. **Password.** |
| `POST /api/prog` | `?remote=<n>`. **Password.** Pairs or unpairs a motor |
| `POST /api/remote/add` | `?address=<hex>` optional. **Password.** Starts not operational |
| `POST /api/remote/flags` | `?remote=<n>&enabled=0\|1&operational=0\|1`. **Password.** |

```bash
curl http://somfy-remote.local/status
curl -X POST "http://somfy-remote.local/api/send?remote=3&command=Up"
```

## MQTT topics

Under `MQTT_DEVICE_ID` from `secrets.h`, except discovery, which must live under whatever
`discovery_prefix` the Home Assistant MQTT integration is configured with —
`HA_DISCOVERY_PREFIX`, `homeassistant/` by default. Get that one wrong and the bridge
connects, publishes happily, and no entity ever appears.

| Topic | Direction |
|---|---|
| `<id>/remote<n>/button` | in — `Up`, `Down`, `My` or `Prog` |
| `<id>/remote<n>/state` | out — `open` / `closed`, retained. Read back at boot to restore position |
| `<id>/remote<n>/my_state` | out — `off` after each My press, retained |
| `<id>/status` | out — `online` / `offline`, retained, last will |
| `<id>/config` | in/out — the configuration document, retained |
| `<id>/code/remote<n>` | in/out — the rolling code mirror, retained |
| `<id>/names` | in — display names from Home Assistant, retained |
| `<id>/health` | out — store diagnostics, retained |
| `<discovery>cover/<id><n>_cover/config` | out — discovery, retained |
| `<discovery>switch/<id><n>_my/config` | out — discovery, retained |
| `<discovery>sensor/<id><n>_code/config` | out — discovery, retained |

The bridge subscribes to `<id>/+/button` once rather than to each remote's topic in turn,
and parses the remote number out of the topic.

The three in/out topics are why a replacement board recovers by itself:
[`docs/configuration.md`](docs/configuration.md).

## Rolling codes

Each emulated remote has a counter, incremented on every press. The motor accepts codes a
short way ahead of the last one it saw, which is what makes a lost transmission harmless —
and a repeated one useless.

The counter is persisted **before** the frame is transmitted, and the frame is not
transmitted at all if that fails. A reboot mid-press therefore skips a code rather than
repeating one, which the motor forgives. `/status` reports the next code for every remote:
a counter that has stopped moving means writes are no longer landing, and the next boot's
commands will be ignored.

Counters are stored in an append-only record store rather than rewritten in place, so a
press costs one 8-byte flash write instead of a 4 KB erase — see [`docs/storage.md`](docs/storage.md).
Every counter is also mirrored to a retained MQTT topic, which is what lets a replacement
board recover them; [`docs/configuration.md`](docs/configuration.md) covers that.

One rule governs all of it: **a counter may only ever move forward.** Home Assistant may
raise one on a board that is behind; nothing may ever lower one.

## The two rings

The console ring holds 80 lines, the fault ring 24, and faults are written to both. The
main log fills with routine traffic — a `health` line every five minutes and every command
sent — so a fault from hours ago would be long evicted by the time anyone went looking.

Lines are stamped with uptime, and consecutive repeats collapse to `(xN)` so one retry loop
cannot evict everything else. The five-minute `health` line carries the fault count, so a
problem is visible from the main log without opening `/errors`. Free heap is reported
whenever it reaches a new low: a healthy board stays quiet, a leaking one shows a steady
descent.

Both rings are RAM and start empty after a restart. `/status` exists for that reason — it
is computed on request rather than remembered, so the build stamp and reset reason cannot
scroll out of anything.

### If a name lookup seems to hang

Use the IP. The ESP8266 mDNS responder answers `A` queries but ignores `AAAA` entirely,
without even a negative reply, so a resolver asking for both waits out its full timeout —
about five seconds — on the IPv6 half before using the IPv4 answer it already had.

## Layout

```
include/        pure logic, header-only, unit tested — frame codec, pulse train,
                topics, record store, configuration document, per-remote state
lib/CC1101/     the radio driver: SPI, registers, transmit mode
src/            peripherals and wiring: radio timing, WiFi/OTA, web UI, MQTT, flash
                plus a standalone self-test with its own build env
test/           desktop unit tests (make test)
tools/          bounded serial capture, privacy scan, config seeding
ha/             Home Assistant side: the automation that publishes display names
docs/           hardware, the RTS protocol, storage, configuration, code standards
local/          gitignored: credentials, IPs, the remotes' RF address
```

`pio test` does not build `src/`, which is why anything worth testing lives in a header
under `include/`. That split is the reason the frame codec, the waveform, the topic names
and the state machine are all verifiable without a radio attached.

## Contributing

[`docs/code-standards.md`](docs/code-standards.md) is binding: minimal code, no dead
things, tests first for anything with a definable input and output, and never leave the
repository dirty. Run `make check` before publishing anything.

## A note on privacy

This repository is written to be publishable. Nothing in it identifies a particular home,
network or installation: no IPs, no credentials, no MAC addresses, and no RF addresses —
that last one is effectively the key to somebody's shutters. Anything site-specific lives
in the gitignored `local/` directory. See RULE 0 in `CLAUDE.md`.

## Credits

The RTS frame layout, checksum, obfuscation and waveform timings are ported from
[`Nickduino/Somfy_Remote`](https://github.com/Nickduino/Somfy_Remote),
[`Legion2/Somfy_Remote_Lib`](https://github.com/Legion2/Somfy_Remote_Lib) and
[Pushstack's protocol notes](https://pushstack.wordpress.com/somfy-rts-protocol/). Years of
reverse engineering went into those and they cannot be re-derived. The CC1101 register
values are the TI datasheet's, by way of the configuration this project's own 2023
predecessor transmitted with. The code around them is this project's own — see
[`docs/somfy-rts.md`](docs/somfy-rts.md).

## Licence

MIT — see [`LICENSE`](LICENSE).
