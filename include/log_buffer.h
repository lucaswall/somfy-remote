#pragma once

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// The recent console lines, kept so they can be read over WiFi once the board is in a case
// with no USB. Fixed size and fixed slots: it has to run for weeks without growing and
// without fragmenting the heap.
#define LOG_LINES 80
#define ERROR_LINES 24
#define LOG_LINE_LEN 100

// Templated on depth so a second, smaller ring for faults costs no duplicated code.
template <uint8_t Lines>
class LogRing {
 public:
  // uptimeSeconds is passed in rather than read here, so this stays pure and testable.
  void push(uint32_t uptimeSeconds, const char *message) {
    // A retry loop repeats the same line every few seconds. Collapsing consecutive
    // repeats stops one persistent fault from evicting everything worth reading.
    if (_count > 0) {
      Slot &last = _slots[(uint8_t)((_head + Lines - 1) % Lines)];
      if (strncmp(last.message, message, LOG_LINE_LEN - 1) == 0) {
        last.seconds = uptimeSeconds;   // when it last happened beats when it started
        if (last.repeats < 0xFFFF) {
          last.repeats++;
        }
        return;
      }
    }

    Slot &slot = _slots[_head];
    strncpy(slot.message, message, LOG_LINE_LEN - 1);
    slot.message[LOG_LINE_LEN - 1] = '\0';
    slot.seconds = uptimeSeconds;
    slot.repeats = 1;

    _head = (uint8_t)((_head + 1) % Lines);
    if (_count < Lines) {
      _count++;
    }
  }

  uint8_t count() const { return _count; }

  // Index 0 is the oldest line held. Renders "[hh:mm:ss] message", with " (xN)" appended
  // when the line stands for several identical events.
  void render(uint8_t index, char *out, size_t len) const {
    if (index >= _count || len == 0) {
      if (len > 0) {
        out[0] = '\0';
      }
      return;
    }

    const uint8_t oldest = (uint8_t)((_head + Lines - _count) % Lines);
    const Slot &slot = _slots[(oldest + index) % Lines];
    const uint32_t s = slot.seconds;

    if (slot.repeats > 1) {
      snprintf(out, len, "[%02u:%02u:%02u] %s (x%u)", (unsigned)(s / 3600),
               (unsigned)((s / 60) % 60), (unsigned)(s % 60), slot.message,
               (unsigned)slot.repeats);
    } else {
      snprintf(out, len, "[%02u:%02u:%02u] %s", (unsigned)(s / 3600),
               (unsigned)((s / 60) % 60), (unsigned)(s % 60), slot.message);
    }
  }

 private:
  struct Slot {
    char message[LOG_LINE_LEN];
    uint32_t seconds;
    uint16_t repeats;
  };

  Slot _slots[Lines] = {};
  uint8_t _head = 0;
  uint8_t _count = 0;
};

typedef LogRing<LOG_LINES> LogBuffer;
typedef LogRing<ERROR_LINES> ErrorBuffer;
