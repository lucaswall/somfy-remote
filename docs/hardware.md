# Hardware

## Board — Wemos D1 mini

ESP8266 with a CH340 USB-serial bridge, micro-USB, 4 MB flash. PlatformIO board id
`d1_mini`.

**The mini is not the D1 R1.** Their pin maps differ, and building with `board = d1`
succeeds and uploads: every `Dn` constant then points at a different GPIO — on the R1, `D1`
is GPIO1, the UART's transmit pin, and `D8` is GPIO0. Nothing in this firmware uses a `Dn`
constant for a real pin, which is what keeps a wrong board id from moving the radio; the
boot banner in `src/main.cpp` prints a `pinmap` line so it is visible anyway.

```
D0=GPIO16  D1=GPIO5   D2=GPIO4   D3=GPIO0   D4=GPIO2
D5=GPIO14  D6=GPIO12  D7=GPIO13  D8=GPIO15
```

| Function | GPIO | Pad |
|---|---|---|
| SPI SCK | 14 | D5 |
| SPI MISO | 12 | D6 |
| SPI MOSI | 13 | D7 |
| Onboard LED (active low) | 2 | D4 |

Boot-strap pins: **GPIO15 must be low at reset**, GPIO0 must be high (low selects the
flash bootloader) and GPIO2 must be high.

## Radio — CC1101 (E07-M1101D or equivalent)

Wire by **GPIO number**, never by silkscreen label:

| CC1101 | GPIO | Pad | Note |
|---|---|---|---|
| GDO0 | 5 | D1 | The data line, and it goes both ways. Transmitting, the chip reads it and we drive it; receiving, the chip drives it and we read it |
| CSN | 15 | D8 | See the warning below |
| SCK | 14 | D5 | Hardware SPI, not reassignable |
| MOSI | 13 | D7 | Hardware SPI |
| MISO | 12 | D6 | Hardware SPI, and the chip's ready handshake |
| GDO2 | — | — | Unused. Left at its reset function |
| VCC | — | 3V3 | **3.3 V only.** 5 V destroys the module |
| GND | — | GND | |

Keep the dupont leads short, 10–20 cm. Long leads are a classic source of flaky SPI.

### CSN on GPIO15 is a compromise

GPIO15 has to be low when the board comes out of reset, and chip select idles **high**.
This works because nothing drives it until the sketch is running and the CC1101 does not
pull it up — but it is the first thing to suspect if the board stops booting entirely: no
serial output at 74880 (`make bootlog`), no blink, looks bricked. A module with a pull-up
on CSN, or one powered before the ESP, will do exactly that.

### Antenna and power

Screw the antenna on before powering up: transmitting into an open port can damage the
output stage. A bare CC1101 draws around 30 mA on transmit, which an ESP board's regulator
handles, but the usual decoupling advice applies — a 10–100 µF electrolytic plus a 100 nF
ceramic across the module's VCC/GND if transmit is unreliable while receive-side register
access is fine.

## Frequency — 433.42 MHz, not 433.92

Somfy RTS sits at **433.42 MHz**. Nearly every other 433 MHz device, and every default in
every CC1101 library, is 433.92. A radio configured at 433.92 will do everything correctly
and be inaudible to the motors.

One consequence worth stating for receive: that offset applies in both directions, so the
receiver is listening about 55 kHz away from where a handheld thinks it is transmitting, and
a handheld's own SAW resonator may be a similar distance the other way. The receive
bandwidth is set generously for exactly that reason, and it is the first thing to widen if a
control cannot be heard.

`lib/CC1101/` writes the frequency word from the module's 26 MHz crystal, and one
empirical offset in `FSCTRL0`. That offset is inherited from the vendor driver the 2023
firmware used, and is what the bridge has been transmitting with since — it is not a
guess, but it is not derived from anything either. If you build this on a module with a
27 MHz crystal, `CRYSTAL_MHZ` in the driver is the value to change, and everything else
follows.

## Bring-up

Run `make radio` before trusting anything else. It probes the chip standalone: reset,
`VERSION`, `PARTNUM`, then a real transmit-mode entry, which only succeeds if the
synthesiser calibrates. Every failure it reports is wiring or power, never software.

- `VERSION` reads `0x00` — MISO is stuck low or the module is unpowered.
- `VERSION` reads `0xFF` — MISO is floating; nothing is answering.
- Answers but will not enter transmit — power, or a crystal that is not oscillating.

**GDO0 used to be the one wire it could not check**, and that is no longer true in the
direction that matters. While only the ESP drives it, nothing reads it back and no register
test can say whether it is connected. But the chip can drive it too — `IOCFG0 = 0x2F` is
"HW to 0", and `0x6F` is the same inverted — so writing those two values and reading GPIO5
is a deterministic continuity test that needs no RF at all. The firmware runs it before it
ever tries to listen.

## What must survive a reflash

1. **The remotes' address.** A motor is paired to an address; drive it from a different one
   and the shutter stops answering, and getting it back means pressing Prog on every motor
   by hand. It is not compiled in — it lives in the retained configuration document, which
   is what a board reads at boot. See `docs/configuration.md`.
2. **The rolling codes.** `make upload` and `make ota` write the application only and leave
   the store's two sectors alone. **`esptool.py erase_flash` does not** — never run it on a
   board you intend to keep. They are also mirrored to MQTT, which is the copy that
   survives the board itself.

The store occupies flash offsets `0x3FA000` and `0x3FB000` on a 4 MB board — the sector the
linker calls `_EEPROM_start` and the unclaimed one below it. Same addresses in every 4 MB
layout the core ships. `docs/storage.md` explains why.
