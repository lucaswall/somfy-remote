#pragma once

#include <stdint.h>

// Where each remote's rolling code lives in EEPROM.
//
// **This layout is not free to change.** A Somfy receiver only accepts codes ahead of the
// one it last saw, so a firmware that reads a remote's counter from the wrong address
// sends a stale code and the shutter ignores it until the counter catches up — in
// practice, until the remote is paired again by hand. The addresses below are the ones
// the 2023 sketch this replaces wrote, and they stay that way.

#define ROLLING_CODE_BYTES 2
#define ROLLING_CODE_MAX_REMOTES 30

// The count slot the old sketch kept at offset 60 is no longer read — the number of
// remotes is compile-time now — but the region stays reserved, so 30 remotes is the cap
// rather than 32.
#define ROLLING_CODE_EEPROM_SIZE 64

inline uint16_t rollingCodeAddress(uint8_t remote) {
  return (uint16_t)(remote * ROLLING_CODE_BYTES);
}

// The stored value is the code to send next, not the last one sent: remotes.cpp writes
// the successor before transmitting, so a board that reboots between the write and the
// send skips a code rather than repeating one. A skipped code the motor accepts; a
// repeated one it ignores.
inline uint16_t rollingCodeNext(uint16_t code) { return (uint16_t)(code + 1); }
