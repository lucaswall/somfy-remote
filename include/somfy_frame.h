#pragma once

#include <stddef.h>
#include <stdint.h>

// The Somfy RTS frame: seven bytes carrying a command, a rolling code and the remote's
// 24-bit address, scrambled by an XOR chain. Pure and header-only so `make test` can
// verify it without a radio — see docs/somfy-rts.md for the wire format.

#define SOMFY_FRAME_LEN 7
#define SOMFY_FRAME_BITS 56

// Byte 0, the so-called encryption key: neither secret nor checked by the motor. Every
// transmitter sends 0xA7, but **a receiver may only test the high nibble** — real handhelds
// vary the low one, sending 0xA1 then 0xA3 on consecutive presses.
#define SOMFY_KEY 0xA7
#define SOMFY_KEY_MASK 0xF0
#define SOMFY_KEY_HIGH 0xA0

// The buttons this bridge exposes. The protocol defines five more (MyUp 0x3, MyDown 0x5,
// UpDown 0x6, SunFlag 0x9, Flag 0xA); they are in docs/somfy-rts.md rather than here,
// because nothing sends them.
enum SomfyCommand {
  SOMFY_MY = 0x1,     // stop, or go to the stored favourite position
  SOMFY_UP = 0x2,
  SOMFY_DOWN = 0x4,
  SOMFY_PROG = 0x8,   // pairing: held on a paired remote, it enrolls the next one
};

// Writes SOMFY_FRAME_LEN obfuscated bytes, ready to be clocked out as-is.
inline void somfyBuildFrame(SomfyCommand command, uint16_t rollingCode, uint32_t address,
                            uint8_t *out) {
  out[0] = SOMFY_KEY;
  out[1] = (uint8_t)((uint8_t)command << 4);   // low nibble is the checksum, filled below
  out[2] = (uint8_t)(rollingCode >> 8);        // rolling code, big endian
  out[3] = (uint8_t)rollingCode;
  out[4] = (uint8_t)(address >> 16);           // remote address, big endian
  out[5] = (uint8_t)(address >> 8);
  out[6] = (uint8_t)address;

  // Checksum: XOR of all fourteen nibbles, computed over the frame while its own nibble
  // is still zero.
  uint8_t checksum = 0;
  for (uint8_t i = 0; i < SOMFY_FRAME_LEN; i++) {
    checksum ^= (uint8_t)(out[i] ^ (out[i] >> 4));
  }
  out[1] |= (uint8_t)(checksum & 0x0F);

  // Obfuscation: each byte is XORed with the already-obfuscated one before it, so a
  // receiver has to unwind the whole chain to read any field.
  for (uint8_t i = 1; i < SOMFY_FRAME_LEN; i++) {
    out[i] ^= out[i - 1];
  }
}

// The command is the raw nibble, not a SomfyCommand: a handheld sends five values this
// firmware never transmits, and narrowing them would make a real press look like corruption.
struct SomfyHeard {
  uint8_t command;
  uint16_t rollingCode;
  uint32_t address;
};

// The inverse of somfyBuildFrame(). The checksum is four bits, so one frame in sixteen of
// pure noise passes it — there is no more entropy in the protocol, which is why a press is
// only believed after two copies agree.
inline bool somfyParseFrame(const uint8_t *frame, SomfyHeard *out) {
  // High nibble only — see SOMFY_KEY. Rejection power is therefore 1/16, not 1/256, which
  // is why the two-copy rule compares address, rolling code *and* command.
  if ((frame[0] & SOMFY_KEY_MASK) != SOMFY_KEY_HIGH) {
    return false;
  }

  // Backwards from the end: each byte was XORed with the *already obfuscated* one before it,
  // so the source of every step is still intact ahead of us.
  uint8_t plain[SOMFY_FRAME_LEN];
  plain[0] = frame[0];
  for (uint8_t i = SOMFY_FRAME_LEN - 1; i >= 1; i--) {
    plain[i] = (uint8_t)(frame[i] ^ frame[i - 1]);
  }

  uint8_t checksum = 0;
  for (uint8_t i = 0; i < SOMFY_FRAME_LEN; i++) {
    checksum ^= (uint8_t)(plain[i] ^ (plain[i] >> 4));
  }
  if ((checksum & 0x0F) != 0) {
    return false;
  }

  out->command = (uint8_t)(plain[1] >> 4);
  out->rollingCode = (uint16_t)(((uint16_t)plain[2] << 8) | plain[3]);
  out->address = ((uint32_t)plain[4] << 16) | ((uint32_t)plain[5] << 8) | plain[6];
  return true;
}

inline const char *somfyCommandName(SomfyCommand command) {
  switch (command) {
    case SOMFY_MY:
      return "My";
    case SOMFY_UP:
      return "Up";
    case SOMFY_DOWN:
      return "Down";
    case SOMFY_PROG:
      return "Prog";
  }
  return "?";
}

// Case-insensitive and length-exact. Exactness is the point: comparing only as far as the
// payload runs — which is what the sketch this replaces did — accepts "u" as Up and "m"
// as My, so a truncated or corrupt payload silently moves a shutter.
inline bool somfyCommandFromText(const char *text, size_t length, SomfyCommand *out) {
  static const struct {
    const char *name;
    SomfyCommand command;
  } TABLE[] = {
      {"My", SOMFY_MY}, {"Up", SOMFY_UP}, {"Down", SOMFY_DOWN}, {"Prog", SOMFY_PROG},
  };

  for (uint8_t i = 0; i < sizeof(TABLE) / sizeof(TABLE[0]); i++) {
    const char *name = TABLE[i].name;
    size_t n = 0;
    while (n < length && name[n] != '\0') {
      const char a = text[n] | 0x20;   // ASCII letters only, which is all these names are
      const char b = name[n] | 0x20;
      if (a != b) {
        break;
      }
      n++;
    }
    if (n == length && name[n] == '\0') {
      *out = TABLE[i].command;
      return true;
    }
  }
  return false;
}
