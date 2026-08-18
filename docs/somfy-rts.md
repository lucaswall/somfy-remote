# Somfy RTS

Everything this firmware needs to know about the protocol. The constants are not
re-derivable from a datasheet — they came from years of community reverse engineering, and
the sources are credited at the end.

## The shape of it

RTS is **one-way**. A remote transmits; the motor acts, and says nothing. There is no
acknowledgement, no state report, no presence beacon. Every consequence in this project
follows from that sentence:

- Home Assistant is told the covers are assumed-state.
- A command lost to interference leaves us confidently wrong until the next one.
- Somebody using the original handheld remote is invisible to us.
- The only proof anything worked is a person watching a shutter move.

## The frame

Seven bytes, transmitted most significant bit first.

| Byte | Contents |
|---|---|
| 0 | `0xA7`. Often called an encryption key; it is neither secret nor checked |
| 1 | Command in the high nibble, checksum in the low nibble |
| 2–3 | Rolling code, big endian |
| 4–6 | The remote's 24-bit address, big endian |

**Checksum.** XOR of all fourteen nibbles of the frame, computed while the checksum nibble
itself is still zero, then written into that nibble. A correct frame therefore XORs down to
zero, which is the test the motor applies.

**Obfuscation.** Each byte from 1 onward is XORed with the already-obfuscated byte before
it. Undoing it means walking the chain backwards. Note that this is a chain, not a mask:
changing one field does not necessarily change every byte after it, because a checksum
change can cancel it out.

### Commands

| Value | Button | Sent by this firmware |
|---|---|---|
| `0x1` | My — stop, or go to the stored favourite position | yes |
| `0x2` | Up | yes |
| `0x3` | My + Up | no |
| `0x4` | Down | yes |
| `0x5` | My + Down | no |
| `0x6` | Up + Down | no |
| `0x8` | Prog — pairing | yes |
| `0x9` | Sun + Flag | no |
| `0xA` | Flag | no |

Only the four in use are in `include/somfy_frame.h`. The rest are here so that adding one
later is a lookup rather than a research project.

### Rolling codes

A 16-bit counter, one per remote address, incremented on every press. The motor remembers
the last code it accepted and takes anything a short way ahead of it, which is what makes a
lost transmission harmless and a repeated code useless.

Two consequences worth stating plainly:

- **Never send a code twice.** Ours is persisted to EEPROM *before* the frame is
  transmitted, so a reboot mid-press skips a code rather than repeating one.
- **Never lose the counter.** A firmware that restarts the count at zero is rejected until
  it climbs back past where it was — in practice, until the motor is paired again by hand.
  See `include/rolling_code.h`, which pins the EEPROM layout for that reason.

### Pairing

Hold Prog on a remote the motor already knows; the shutter jogs to acknowledge. Press Prog
on the new remote within a couple of seconds and it is enrolled. Sending Prog to a motor
that already knows this address **un**pairs it. This is why the web UI asks before sending
one.

## The waveform

OOK at **433.42 MHz** — see `docs/hardware.md`, this is not 433.92. The bit period is
1280 µs, made of two 640 µs half-symbols.

One press is:

```
wake-up      9415 µs high, 9565 µs low, then 80 ms of silence
frame 1      2 hardware sync pairs
frames 2-5   7 hardware sync pairs each
             ... each followed by 30 ms of silence
```

and each frame is:

```
hardware sync   N × (2560 µs high, 2560 µs low)
software sync   4550 µs high, 640 µs low
data            56 bits, Manchester, MSB first
silence         415 µs low
```

**The Manchester convention is inverted** from the usual one: a `1` is 640 µs low then
640 µs high, a `0` is high then low. Getting this backwards produces a signal with a
perfectly valid shape and no meaning, and nothing will ever tell you — which is why
`test_somfy_pulses` asserts it explicitly.

The repeats are not optional. A receiver samples the band rather than listening
continuously, so a single copy of a frame is often sent while it is not looking. Five
copies of every press is what a real remote does.

A press therefore occupies the air for roughly 800 ms. That is why commands are queued
(`include/command_queue.h`) instead of being transmitted from whichever callback asked for
them.

## Credits

The frame layout, the checksum, the obfuscation and every timing above are ported from
work by others:

- [`Nickduino/Somfy_Remote`](https://github.com/Nickduino/Somfy_Remote) — the original
  Arduino implementation, and the source of the timing constants.
- [`Legion2/Somfy_Remote_Lib`](https://github.com/Legion2/Somfy_Remote_Lib) (Apache-2.0) —
  the library the 2023 firmware this replaces depended on.
- Pushstack's [Somfy RTS protocol
  notes](https://pushstack.wordpress.com/somfy-rts-protocol/) — the reverse engineering
  everything else rests on.

The code in this repository is its own; the protocol constants are theirs.
