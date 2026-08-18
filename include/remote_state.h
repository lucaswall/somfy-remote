#pragma once

#include <stdint.h>

#include "somfy_frame.h"

// What one shutter is doing, as far as we can tell.
//
// RTS is one-way: the motor never reports anything — not its position, not that it heard
// us. Everything here is inferred from what we transmitted, which is why Home Assistant is
// told these entities are assumed-state.
enum CoverPosition {
  COVER_UNKNOWN,
  COVER_OPEN,
  COVER_CLOSED,
};

class RemoteState {
 public:
  // Called once per command actually transmitted, whoever asked for it — Home Assistant,
  // the web UI, or the serial console — so every path lands on the same state.
  void record(SomfyCommand command) {
    _last = command;
    _version++;

    if (command == SOMFY_UP) {
      _position = COVER_OPEN;
    } else if (command == SOMFY_DOWN) {
      _position = COVER_CLOSED;
    }
    // My stops the shutter wherever it happens to be and Prog does not move it. Neither
    // gives a position we could report, so the last one stands.
  }

  CoverPosition position() const { return _position; }
  SomfyCommand last() const { return _last; }

  // Advances on every command, not only on a change of position: the My button has to
  // report back after each press, and two Ups in a row are two events.
  uint32_t version() const { return _version; }

 private:
  CoverPosition _position = COVER_UNKNOWN;
  SomfyCommand _last = SOMFY_MY;
  uint32_t _version = 0;
};

inline const char *coverPositionName(CoverPosition position) {
  switch (position) {
    case COVER_OPEN:
      return "open";
    case COVER_CLOSED:
      return "closed";
    case COVER_UNKNOWN:
      break;
  }
  return "unknown";
}
