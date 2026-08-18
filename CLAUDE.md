# somfy-remote

ESP8266 + CC1101 bridge that puts Somfy RTS shutters on Home Assistant over MQTT, by
emulating as many handheld remotes as there are shutters. See `README.md` for what it does,
`docs/hardware.md` for wiring and `docs/somfy-rts.md` for the protocol.

---

## RULE 0 — THIS REPOSITORY IS PUBLIC

Everything here is world-readable, including the full commit history, and a value pushed
once cannot be recalled — it can be cloned before it is removed. This rule outranks
convenience, and it applies to code, comments, commit messages, docs, and issue text.

It governs the repository. It says nothing about what the device serves on its own
network: the web UI showing shutter state to a browser on the LAN is not a RULE 0 matter.

**Never commit, in any file or commit message:**

- WiFi SSIDs or passwords, MQTT usernames/passwords, API tokens, OTA passwords
- IP addresses of real hosts — LAN, VPN/Tailscale, or public. Use `<HUB_IP>` or RFC 5737
  documentation addresses (`192.0.2.x`) in examples
- Hostnames of real machines, and the owner's home network topology
- MAC addresses, and the ESP's chip ID
- **`SOMFY_ADDRESS_BASE` — the emulated remotes' RF address.** This is the credential of
  the shutters on a real house: anyone within radio range who knows it can open every one
  of them. It is the single most sensitive value in the project
- Home Assistant instance URLs, long-lived tokens, or entity registries dumped verbatim
- Photos of the house, floor plans, geolocation, anything identifying the address
- Personal names, email addresses, phone numbers, purchase/invoice references —
  **except the copyright line in `LICENSE`**, which names the author deliberately. Do not
  "clean" it; a licence needs an identifiable holder to mean anything.

**Where site-specific values go instead:** `local/` — the whole directory is gitignored
except its README. Put real IPs, credentials, the remote address and personal notes in
`local/site.md`. Never `git add -f` anything under `local/`. Secrets that must reach the
firmware go in `include/secrets.h` (gitignored), with `include/secrets.h.example`
committed carrying placeholders only.

**Before every commit:** if a value came from the real installation rather than from a
datasheet, it belongs in `local/`, not in the tree. When in doubt, leave it out and put a
placeholder. `make check` catches the common shapes; it cannot catch everything, so read
your own diff.

**Keep the docs generic.** This targets Home Assistant, not one particular Home Assistant.
Write "your broker", "your HA instance", "the hub's IP" — never the real ones. A reader
with the same hardware should be able to follow the docs without knowing anything about
the author's house.

---

## Two things that must not change

This firmware replaces one that has been running since 2023, on a board that is already
installed and already paired.

1. **The MQTT topics, unique ids and device names** in `include/topics.h`. Home Assistant
   keys its entities on them; changing one orphans a dozen covers and every automation
   that mentions them. `test/test_topics/` pins the strings.
2. **The EEPROM rolling code layout** in `include/rolling_code.h`. Read a counter from the
   wrong address and the shutter ignores the command until somebody re-pairs the motor by
   hand. `test/test_rolling_code/` pins the addresses.

Both files say so at the top. Neither is a design choice that is still open.

## Hardware

Wemos **D1 mini** (ESP8266, 4 MB flash) + CC1101 433 MHz transceiver.

PlatformIO board id is `d1_mini`. **NOT `d1`** — that is the R1, an Uno-shaped board with a
different pin map; it compiles and uploads cleanly while every pin number is silently
wrong.

### Pin map — the D1 mini is not the R1

```
D0=GPIO16  D1=GPIO5   D2=GPIO4   D3=GPIO0   D4=GPIO2
D5=GPIO14  D6=GPIO12  D7=GPIO13  D8=GPIO15
```

- Radio: CSN=GPIO15, GDO0=GPIO5, SCK=GPIO14, MOSI=GPIO13, MISO=GPIO12.
- `LED_BUILTIN` = GPIO2 (pad D4), **active low**.
- GPIO15 (pad D8) is chip select *and* a boot-strap pin that must be low at reset. A board
  that will not boot at all is this, not software — see `docs/hardware.md`.

**Always reason in GPIO numbers.** Every D1 R1 and NodeMCU tutorial gives different
D-numbers for the same pin.

## Commands

`make help` lists everything. The common ones:

| Task | Command |
|---|---|
| Find the port | `make ports` |
| Build | `make build` |
| Flash | `make upload` |
| Flash over WiFi | `make ota OTA_HOST=<name-or-ip>` |
| Flash, then read the banner | `make run` |
| Unit tests (desktop) | `make test` |
| CC1101 self-test on hardware | `make radio` |
| Read serial (safe from a tool call) | `make log` |
| Read the boot ROM at 74880 | `make bootlog` |
| RULE 0 scan (always before publishing) | `make check` |
| Symbol index for clangd | `make compiledb` |
| Serial monitor (human, own terminal only) | `pio device monitor` |

## The serial port is a single-holder resource

One process owns `/dev/cu.*` at a time. A live monitor makes upload fail with
`Could not exclusively lock port ... [Errno 35]`. PlatformIO will not stop it for you
(platformio-core#384, wontfix). Stop any reader before flashing, and never end a session
with one running. `lsof /dev/cu.usbserial-*` finds the holder.

## Never run `pio device monitor` from a tool call

It dies with `termios.error: (25, 'Inappropriate ioctl for device')` whenever stdin is not
a TTY, which is always true for an agent Bash call (platformio-core#5113, open). A
foreground call that times out gets backgrounded rather than killed, leaving an orphan
holding the port. `.claude/settings.json` denies it. Do not pass `-t monitor` to `pio run`
either. Use `make log` (`tools/serial_log.py`), which reads for a bounded time and always
closes the port.

## Three different baud rates

- **115200** — our sketches (`Serial.begin(115200)`, `monitor_speed = 115200`)
- **74880** — the ESP8266 boot ROM banner. Garbage at 115200 during the first second after
  reset is expected, not a fault. Read it with `make bootlog`
- **9600** — `pio device monitor`'s own default when no speed is given. Cause of most
  "garbage on the monitor" reports

## No JTAG

The ESP8266 has no on-chip debug and `d1_mini.json` declares no debug block; `pio debug`
does not work and no probe helps. Debugging is serial prints, the
`esp8266_exception_decoder` monitor filter, and offline `addr2line`.

## macOS notes

- **Apple Silicon needs Rosetta 2.** The xtensa compiler is x86_64-only and no arm64 build
  exists. `Bad CPU type in executable` / scons `Error 126` means it is missing:
  `softwareupdate --install-rosetta --agree-to-license`
- **Do not install a CH340 driver.** macOS ships Apple's own `AppleUSBCHCOM`, which matches
  this chip. WCH's driver adds a dead duplicate port and, on Apple Silicon, breaks the
  DTR/RTS auto-reset that flashing depends on. A `/dev/cu.wchusbserial*` node means a
  third-party driver is installed and should be removed

## Conventions

**`docs/code-standards.md` is binding on every change.** Read it before writing code.
The rules that get broken most often:

- **Minimal.** Smallest thing that works. No abstraction or option for a use case that
  does not exist yet. Comments explain **why**, not what.
- **No dead things.** No commented-out code, no unused files, headers, targets or
  instructions. Delete them in the same commit that makes them dead — a future agent
  cannot distinguish a deliberate leftover from an oversight.
- **Test first** for anything with a definable input and output. Pure logic goes in a
  header under `include/` and is tested by `make test`; `src/` is hardware and is verified
  by looking at the board.
- **Never leave the repo dirty.** Commit and push a finished set of changes before moving
  on, and decide tracked-or-gitignored for a new file the moment it appears.
- Verify on hardware before committing anything the tests cannot reach.
- No git tags. Use the commit log to find a working state.
