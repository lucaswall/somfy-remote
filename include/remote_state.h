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
  // Called once per command actually transmitted, whoever asked for it — Home Assistant
  // or the web UI — so every path lands on the same state.
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

  // A press somebody else made, overheard on the air. Separate from record() because the
  // command is a raw nibble rather than one of the four this firmware transmits: a handheld
  // sends My+Up, My+Down, Up+Down, Sun and Flag as well (docs/somfy-rts.md), and narrowing
  // them earlier would make a real button press look like corruption.
  //
  // The version advances exactly as it does for our own commands, because the point of
  // hearing a handheld is that Home Assistant stops being confidently wrong about the
  // cover — and nothing publishes until the version moves.
  void observe(uint8_t command) {
    _version++;

    if (command == SOMFY_UP) {
      _position = COVER_OPEN;
      _last = SOMFY_UP;
    } else if (command == SOMFY_DOWN) {
      _position = COVER_CLOSED;
      _last = SOMFY_DOWN;
    } else if (command == SOMFY_MY) {
      _last = SOMFY_MY;   // the My switch reports itself off after presses it did not cause
    }
    // Everything else — My+Up, My+Down, Up+Down, Sun, Flag — moves the shutter in ways this
    // firmware cannot predict a position for. The last one stands, which is the same answer
    // record() gives for My.
  }

  // Seeds the position from Home Assistant's retained copy at boot. Deliberately not
  // record(): nothing was transmitted, so the version must not advance and no press should
  // be implied. Without this the device forgets where every shutter is on each reboot and
  // reports `unknown` while Home Assistant still shows the truth, which is two answers to
  // one question.
  void restore(CoverPosition position) {
    if (_position == COVER_UNKNOWN) {
      _position = position;
    }
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
