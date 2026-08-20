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
| 0 | Often called an encryption key; it is neither secret nor checked. See below |
| 1 | Command in the high nibble, checksum in the low nibble |
| 2–3 | Rolling code, big endian |
| 4–6 | The remote's 24-bit address, big endian |

**The key byte must not be tested at all.** Every transmitter in the wild is documented as
sending `0xA7`, and handhelds do not even hold that constant: captures show one remote
sending `0xA1` then `0xA3` on consecutive presses, and ESPSomfy-RTS transmits
`0xA0 | (rolling code & 0x0F)`. So the received value was once compared against its high
nibble only.

That is still too strict. **The wall switches in this installation send `0x8F` and `0xF6`** —
sixteen consecutive frames, every checksum clean, one stable address per device, commands
that match the button pressed. A high-nibble comparison made every one of them unhearable,
and because a rejected frame was counted as a checksum failure, the symptom read as noise
rather than as a device being refused.

This firmware therefore reports byte 0 and judges nothing. What keeps noise out is the
two-copy rule below, not this byte.

**Checksum.** XOR of all fourteen nibbles of the frame, computed while the checksum nibble
itself is still zero, then written into that nibble. A correct frame therefore XORs down to
zero, which is the test the motor applies.

**Obfuscation.** Each byte from 1 onward is XORed with the already-obfuscated byte before
it. Undoing it means walking the chain backwards. Note that this is a chain, not a mask:
changing one field does not necessarily change every byte after it, because a checksum
change can cancel it out.

### The checksum cannot see a single-bit error

Worth knowing before trusting a received frame, and not obvious from either half on its own.
A bit flipped in transit unwinds into the **same bit flipped in two adjacent plaintext
bytes** — `plain[i] = frame[i] ^ frame[i-1]` and `plain[i+1] = frame[i+1] ^ frame[i]` — and
two identical contributions to a checksum that is only an XOR of nibbles cancel exactly. The
frame still sums to zero.

Only byte 6 is protected, because nothing follows it to cancel against. Forty-eight of the
fifty-six possible single-bit flips decode cleanly, and forty-four of those come out as a
*different* address, rolling code or command. `test_somfy_frame` asserts both numbers so
they cannot quietly get worse.

Byte 0 is worth understanding here. It reaches no reported field directly, but it XORs into
`plain[1]`, so a flip in its **high** nibble changes the decoded command while leaving the
address and rolling code intact. Dropping the key comparison is what admits those four
cases; the two-copy rule is what makes them harmless.

The consequence is the whole design of the receive path: a corrupted frame does not look
corrupt, it looks like another remote. A press is therefore only believed once two copies
agree byte for byte, which the five repeats of every press make cheap.

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

- **Never send a code twice.** Ours is persisted *before* the frame is transmitted, and
  the frame does not go out if the write cannot be verified — so a reboot mid-press skips
  a code rather than repeating one.
- **Never lose the counter, and never lower it.** A firmware that restarts the count at
  zero is rejected until it climbs back past where it was — in practice, until the motor
  is paired again by hand. `include/record_store.h` states the rule; `docs/storage.md`
  describes how it is held.

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
