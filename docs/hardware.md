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
| GDO0 | 5 | D1 | The data line. In transmit the chip reads it; we drive it |
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

**GDO0 is the one wire it cannot check.** The chip never reads it back, so no register test
can tell whether it is connected, mis-wired or shorted. Verify that one by eye.

## Replacing the 2023 firmware on a board that is already installed

Two things on that board must survive, and neither is in this repository:

1. **The remotes' address** (`SOMFY_ADDRESS_BASE`). A motor is paired to an address. Flash
   a firmware with a different one and the shutter simply stops answering, and getting it
   back means pressing Prog on every motor by hand.
2. **The rolling codes in EEPROM.** A normal `make upload` writes the application only and
   leaves the EEPROM sector alone, so they survive. `esptool.py erase_flash` does not —
   never run it on a board you intend to keep.

If the address has been lost, try the serial console first: a firmware that logs its
configuration at boot will have printed it, and reading it back costs nothing. Otherwise
dump the old flash before overwriting it (`esptool.py read_flash 0 0x400000 old.bin`) and
look for it in the image — a 24-bit constant is a 4-byte-aligned little-endian word with a
zero high byte — or receive a press off the air with an RTL-SDR or a CC1101 in receive
mode, which yields the current rolling code as well. Dump the flash **first** either way:
it costs a minute, and it is the only copy of the EEPROM sector.

On a 4 MB ESP8266 that sector is at flash offset `0x3FB000` (`_EEPROM_start = 0x405fb000`),
and it is the same address in every 4 MB linker layout the core ships — so a rebuild with a
different filesystem size still finds the counters where the old firmware left them.
