#pragma once

#include <stdint.h>

// Unsigned subtraction, so this stays correct across the millis() rollover at ~49.7 days.
// Comparing (now >= last + interval) instead would stall the board for a full cycle.
inline bool elapsed(uint32_t now, uint32_t last, uint32_t interval) {
  return (uint32_t)(now - last) >= interval;
}
