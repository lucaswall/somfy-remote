#pragma once

#include <stdint.h>

#include "somfy_frame.h"

// What one shutter is doing, as far as we can tell.
//
// RTS is one-way: the motor never reports anything — not its position, not that it heard us.
// Everything here is inferred from a command, whether this bridge sent it or overheard
// somebody else's remote sending it, which is why the entities are declared assumed-state.
enum CoverPosition {
  COVER_UNKNOWN,
  COVER_OPEN,
  COVER_CLOSED,
  COVER_OPENING,
  COVER_CLOSING,
};

// How long a shutter takes to travel end to end, when nothing else is configured. A property
// of the motor and the drop, so every remote carries its own and this is only the fallback.
#define COVER_TRAVEL_MS 22000

// Home Assistant's convention, and the one the percentage is expressed in throughout.
#define COVER_PCT_OPEN 100
#define COVER_PCT_CLOSED 0
#define COVER_PCT_UNKNOWN -1

class RemoteState {
 public:
  // Measured per shutter and configured from the settings page. Zero is refused rather than
  // stored: it would make every travel instantaneous and the percentage meaningless.
  void setTravelMs(uint32_t travelMs) {
    if (travelMs > 0) {
      _travelMs = travelMs;
    }
  }
  uint32_t travelMs() const { return _travelMs; }

  // A command this bridge transmitted.
  void record(SomfyCommand command, uint32_t nowMs) {
    _last = command;
    apply((uint8_t)command, nowMs);
  }

  // A press somebody else made, overheard on the air. The command is a raw nibble: a handheld
  // also sends My+Up, My+Down, Up+Down, Sun and Flag.
  void observe(uint8_t command, uint32_t nowMs) { apply(command, nowMs); }

  // Lands a shutter that has finished travelling. Nothing else moves a cover on from opening
  // or closing.
  void tick(uint32_t nowMs) {
    if (!_travelling || (int32_t)(nowMs - _arrivesAt) < 0) {
      return;
    }
    _travelling = false;
    settle(_toPct);
    _version++;
  }

  // Where the shutter is now, 0 shut to 100 open, or COVER_PCT_UNKNOWN. Interpolated while
  // travelling, which is the whole reason the travel time is worth measuring.
  int16_t percent(uint32_t nowMs) const {
    if (!_travelling) {
      return _pct;
    }
    const uint32_t elapsed = nowMs - _travelStart;
    if (_duration == 0 || elapsed >= _duration) {
      return _toPct;
    }
    const int32_t span = (int32_t)_toPct - (int32_t)_fromPct;
    return (int16_t)((int32_t)_fromPct + span * (int32_t)elapsed / (int32_t)_duration);
  }

  // Seeds the position from Home Assistant's retained copy at boot. Deliberately not
  // record(): nothing was transmitted, so the version must not advance and no press should be
  // implied. Without it the device forgets where every shutter is on each reboot and reports
  // `unknown` while Home Assistant still shows the truth, which is two answers to one
  // question.
  void restore(CoverPosition position) {
    if (_position != COVER_UNKNOWN) {
      return;
    }
    if (position == COVER_OPEN) {
      settle(COVER_PCT_OPEN);
    } else if (position == COVER_CLOSED) {
      settle(COVER_PCT_CLOSED);
    }
  }

  // The retained percentage, which is finer than the state: a shutter stopped half way
  // retains `open`, and only this says half way.
  void restorePercent(int16_t pct) {
    if (_travelling || pct < 0 || pct > COVER_PCT_OPEN) {
      return;
    }
    if (_position == COVER_UNKNOWN || _pct == COVER_PCT_UNKNOWN) {
      settle(pct);
    } else if (_position != COVER_CLOSED && pct > COVER_PCT_CLOSED) {
      // Both agree it is not shut, so take the finer answer without letting a stale
      // percentage contradict a state we already restored.
      _pct = pct;
    }
  }

  CoverPosition position() const { return _position; }
  SomfyCommand last() const { return _last; }
  bool travelling() const { return _travelling; }

  // Advances on every command, not only on a change of position: the My button has to report
  // back after each press, and two Ups in a row are two events.
  uint32_t version() const { return _version; }

 private:
  void apply(uint8_t command, uint32_t nowMs) {
    _version++;

    if (command == SOMFY_UP) {
      _last = SOMFY_UP;
      start(COVER_PCT_OPEN, nowMs);
    } else if (command == SOMFY_DOWN) {
      _last = SOMFY_DOWN;
      start(COVER_PCT_CLOSED, nowMs);
    } else if (command == SOMFY_MY) {
      _last = SOMFY_MY;
      if (_travelling) {
        const int16_t here = percent(nowMs);
        _travelling = false;
        settle(here);
      } else {
        // Every motor here has its favourite position set to fully closed, so My on a
        // stationary shutter drives it shut. A fact about these motors rather than the
        // protocol — elsewhere the favourite sits somewhere in the middle, and this would
        // have to become a setting.
        start(COVER_PCT_CLOSED, nowMs);
      }
    }
    // Prog moves nothing, and the rest move the shutter in ways no position can be computed
    // from, so the last one stands.
  }

  void start(int16_t targetPct, uint32_t nowMs) {
    // An unknown shutter is assumed to be at the far end, so the travel takes the full time
    // and the estimate is right by the end of it however wrong it was at the start.
    int16_t from = _travelling ? percent(nowMs) : _pct;
    if (from == COVER_PCT_UNKNOWN) {
      from = (targetPct == COVER_PCT_OPEN) ? COVER_PCT_CLOSED : COVER_PCT_OPEN;
    }
    if (from == targetPct) {
      _travelling = false;
      settle(targetPct);
      return;
    }

    const int16_t distance = (int16_t)(from > targetPct ? from - targetPct : targetPct - from);
    _fromPct = from;
    _toPct = targetPct;
    _travelStart = nowMs;
    _duration = _travelMs * (uint32_t)distance / COVER_PCT_OPEN;
    _arrivesAt = nowMs + _duration;
    _travelling = true;
    _position = (targetPct == COVER_PCT_OPEN) ? COVER_OPENING : COVER_CLOSING;
  }

  // A cover without a position calls anything not fully shut open, and so must this: it is
  // the same answer Home Assistant derives for is_closed.
  void settle(int16_t pct) {
    _pct = pct;
    _position = (pct <= COVER_PCT_CLOSED) ? COVER_CLOSED : COVER_OPEN;
  }

  CoverPosition _position = COVER_UNKNOWN;
  SomfyCommand _last = SOMFY_MY;
  uint32_t _version = 0;
  uint32_t _travelMs = COVER_TRAVEL_MS;
  uint32_t _travelStart = 0;
  uint32_t _duration = 0;
  uint32_t _arrivesAt = 0;
  int16_t _pct = COVER_PCT_UNKNOWN;
  int16_t _fromPct = 0;
  int16_t _toPct = 0;
  bool _travelling = false;
};

inline const char *coverPositionName(CoverPosition position) {
  switch (position) {
    case COVER_OPEN:
      return "open";
    case COVER_CLOSED:
      return "closed";
    case COVER_OPENING:
      return "opening";
    case COVER_CLOSING:
      return "closing";
    case COVER_UNKNOWN:
      break;
  }
  return "unknown";
}
