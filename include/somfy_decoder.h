#pragma once

#include <stdint.h>
#include <string.h>

#include "somfy_frame.h"
#include "somfy_pulses.h"

// Turning what the radio heard back into a press: a state machine over edge intervals, and
// the rule that decides when enough copies of a frame have arrived to believe it.
//
// Pure and header-only, so `make test` can drive it from the transmitter's own pulse train
// (test_somfy_decode) instead of from a radio. That round trip is the only verification
// available before hardware exists, and it is a strong one: the encoder is already pinned
// against golden frames, so a decoder that agrees with it agrees with a real remote.
//
// **This runs in the main loop, never in the interrupt handler.** The ISR timestamps edges
// and nothing else. Every reference implementation of this protocol decodes inside the ISR;
// they run on chips that are not also holding up a WiFi stack on the same core.

// ±30 %, the tolerance the closest prior art uses against real handhelds. Wide because the
// timings in somfy_pulses.h came from somebody else's remotes and this house's may differ —
// docs/somfy-rts.md credits the sources, none of which is a datasheet.
#define SOMFY_TOLERANCE_PERCENT 30

// Two hardware sync pairs, counted as four intervals because that is what an edge stream
// presents. Two and not seven: a press opens with SOMFY_FIRST_SYNC pairs and only the four
// repeats carry SOMFY_REPEAT_SYNC, so demanding seven would discard the first frame of
// every press — the one that arrives 150 ms before any other.
#define SOMFY_MIN_SYNC_INTERVALS (2 * SOMFY_FIRST_SYNC)

// Anything longer than this ends whatever was in progress. Two constraints fix it, and the
// window between them is narrower than it looks: it must clear the longest interval a frame
// contains — the 4550 µs software sync, which is 5915 µs once a remote running 30 % slow is
// allowed for — and it must sit below the 9415 µs wake-up pulse, which is the first thing a
// press sends and the thing this most needs to reset on.
#define SOMFY_GAP_US 7000

inline bool somfyNear(uint32_t measured, uint32_t expected) {
  const uint32_t slack = expected * SOMFY_TOLERANCE_PERCENT / 100;
  return measured + slack >= expected && measured <= expected + slack;
}

// Feeds on (level, duration) pairs — "the line was high for 640 µs" — and reports a frame
// once 56 bits have arrived and the checksum holds.
class SomfyDecoder {
 public:
  void reset() {
    _inData = false;
    _syncIntervals = 0;
    _bits = 0;
    _waitingHalf = false;
    _haveLevel = false;
  }

  bool feed(bool high, uint32_t microseconds, SomfyHeard *out) {
    // The level is recorded, counted and *not* acted on. See the note above emit().
    if (_haveLevel && high == _lastHigh) {
      _levelRepeats++;
    }
    _haveLevel = true;
    _lastHigh = high;

    if (microseconds > SOMFY_GAP_US) {
      reset();
      _haveLevel = true;
      _lastHigh = high;
      return false;
    }

    if (_inData) {
      return feedData(microseconds, out);
    }
    feedSync(microseconds);
    return false;
  }

  // Frames abandoned part-way. A receiver that decodes nothing looks identical to a quiet
  // house, and this is the counter that tells them apart.
  uint16_t aborted() const { return _aborted; }

  // How often two consecutive intervals arrived at the same level, which cannot happen if
  // every edge was seen. It is the sharpest measure available of how much the front end is
  // dropping: near zero means a clean line, and a large fraction means the data pin is
  // chattering faster than anything downstream can follow.
  uint16_t levelRepeats() const { return _levelRepeats; }

  // True once for each frame that just began, so a diagnostic capture can start where the
  // data does. Without it the sync burst fills most of the buffer and the frame is cut off
  // at the far end — which reads exactly like a corrupt frame and is not one.
  bool takeFrameStart() {
    const bool started = _frameStarted;
    _frameStarted = false;
    return started;
  }

  // True once for each frame that ran all the way to 56 bits and then failed its checksum.
  //
  // Deliberately not "any abandoned frame": noise finds a false sync every few seconds and
  // gives up within a handful of intervals, and a diagnostic that freezes on the first of
  // those never sees the press it was armed for. A frame that reached full length and only
  // then failed is the one that has something to say.
  bool takeChecksumFailure() {
    const bool failed = _checksumFailed;
    _checksumFailed = false;
    return failed;
  }

 private:
  void abandon() {
    if (_inData) {
      _aborted++;
    }
    reset();
  }

  // Hardware sync is a burst of 2560 µs half-periods; the software sync that follows is a
  // single 4550 µs high. The hardware test comes first because at ±30 % the two windows
  // overlap between 3185 and 3328 µs, and mistaking a software sync for one more hardware
  // sync only costs this frame — the reverse would start reading data from the middle of a
  // sync burst.
  void feedSync(uint32_t microseconds) {
    if (somfyNear(microseconds, SOMFY_HW_SYNC_US)) {
      if (_syncIntervals < 0xFF) {
        _syncIntervals++;
      }
      return;
    }
    if (somfyNear(microseconds, SOMFY_SW_SYNC_HIGH_US) &&
        _syncIntervals >= SOMFY_MIN_SYNC_INTERVALS) {
      memset(_frame, 0, sizeof(_frame));
      _inData = true;
      _bits = 0;
      _waitingHalf = false;
      _bit = false;   // the toggle's seed: the first full symbol after sync is a 1
      _frameStarted = true;
      return;
    }
    _syncIntervals = 0;
  }

  // Manchester, inverted: a 1 is low then high. Bits come from the *durations* alone, with a
  // running toggle seeded at the software sync — a full symbol flips the current bit and
  // emits it, and a pair of half symbols emits it again unchanged.
  //
  // An earlier version of this derived the bit from the level of the interval instead, which
  // is equivalent on a clean stream and looked strictly better: a toggle that slips yields a
  // whole frame of plausible garbage, while a level that repeats is a lost edge you can
  // catch. On this hardware it decoded nothing at all.
  //
  // The reason is the glitch filter in front of it. It drops every edge closer than
  // SOMFY_SYMBOL_US * 0.7 to the last one it kept, and under a noisy OOK line that is an
  // arbitrary number of edges rather than a tidy pair — so the recorded level is very nearly
  // random. Measured on the installed board: the level alternated on 19 % of transitions
  // where it must alternate on 100 %, biased high because a chattering data line sits high.
  //
  // Every reference implementation of this protocol decodes from durations, and this is why.
  // The level is still recorded, because levelRepeats() is the number that found this.
  bool feedData(uint32_t microseconds, SomfyHeard *out) {
    if (somfyNear(microseconds, 2 * SOMFY_SYMBOL_US)) {
      if (_waitingHalf) {
        abandon();   // a full symbol cannot follow a lone half: the phase is wrong
        return false;
      }
      _bit = !_bit;
      return emit(_bit, out);
    }
    if (somfyNear(microseconds, SOMFY_SYMBOL_US)) {
      if (!_waitingHalf) {
        _waitingHalf = true;
        return false;
      }
      _waitingHalf = false;
      return emit(_bit, out);
    }
    abandon();
    return false;
  }

  bool emit(bool bit, SomfyHeard *out) {
    if (bit) {
      _frame[_bits / 8] |= (uint8_t)(0x80u >> (_bits % 8));
    }
    if (++_bits < SOMFY_FRAME_BITS) {
      return false;
    }

    const bool valid = somfyParseFrame(_frame, out);
    if (!valid) {
      _aborted++;
      _checksumFailed = true;
    }
    reset();
    return valid;
  }

  uint8_t _frame[SOMFY_FRAME_LEN] = {0};
  uint16_t _aborted = 0;
  uint16_t _levelRepeats = 0;
  bool _bit = false;
  bool _frameStarted = false;
  bool _checksumFailed = false;
  uint8_t _syncIntervals = 0;
  uint8_t _bits = 0;
  bool _inData = false;
  bool _waitingHalf = false;
  bool _haveLevel = false;
  bool _lastHigh = false;
};

// --- from frames to presses ---------------------------------------------------------------

// A press is one button push. Somfy sends each one five times (docs/somfy-rts.md), so a
// receiver that reports every frame reports every press five times.
//
// Copies are collapsed by (address, rolling code) rather than by a timer, because the
// rolling code *is* the press identifier: the five copies of one push carry the same one
// and the next push carries the next. A pure time window would merge two quick presses and
// split one slow burst.
#define SOMFY_BURST_MS 1200

// Two agreeing copies before a press is believed. The frame checksum is four bits and
// cannot even see a single-bit error (test_somfy_frame pins this: 40 of 56 flips decode
// clean, as a *different* address), so one frame is not evidence of anything. Two identical
// copies is.
#define SOMFY_BURST_COPIES 2

// Four remotes mid-burst at once. One slot is not enough: a neighbour's remote, or two
// people pressing during the naming walk, would thrash a single slot and neither press
// would ever reach its second copy.
#define SOMFY_BURST_SLOTS 4

struct SomfyPress {
  uint32_t address;
  uint16_t rollingCode;
  uint8_t command;
};

class SomfyPressAssembler {
 public:
  // True exactly once per press, on the copy that confirms it — roughly 150 ms into the
  // burst rather than at the end of it. `nowMs` is millis(); the arithmetic is unsigned, so
  // the rollover is not a special case.
  bool feed(const SomfyHeard &heard, uint32_t nowMs, SomfyPress *out) {
    Slot *slot = find(heard.address);
    if (slot == nullptr) {
      slot = evictOldest(nowMs);
    }

    const bool sameBurst = slot->used && slot->address == heard.address &&
                           slot->rollingCode == heard.rollingCode &&
                           (uint32_t)(nowMs - slot->at) <= SOMFY_BURST_MS;
    if (!sameBurst) {
      *slot = {heard.address, heard.rollingCode, heard.command, 1, nowMs, true};
      return false;
    }

    slot->at = nowMs;
    if (slot->copies >= 0xFF) {
      return false;
    }
    if (++slot->copies != SOMFY_BURST_COPIES) {
      return false;   // later copies of a press already reported
    }
    out->address = heard.address;
    out->rollingCode = heard.rollingCode;
    out->command = slot->command;
    return true;
  }

 private:
  struct Slot {
    uint32_t address;
    uint16_t rollingCode;
    uint8_t command;
    uint8_t copies;
    uint32_t at;
    bool used;
  };

  Slot *find(uint32_t address) {
    for (uint8_t i = 0; i < SOMFY_BURST_SLOTS; i++) {
      if (_slots[i].used && _slots[i].address == address) {
        return &_slots[i];
      }
    }
    return nullptr;
  }

  Slot *evictOldest(uint32_t nowMs) {
    Slot *oldest = &_slots[0];
    for (uint8_t i = 0; i < SOMFY_BURST_SLOTS; i++) {
      if (!_slots[i].used) {
        return &_slots[i];
      }
      if ((uint32_t)(nowMs - _slots[i].at) > (uint32_t)(nowMs - oldest->at)) {
        oldest = &_slots[i];
      }
    }
    return oldest;
  }

  Slot _slots[SOMFY_BURST_SLOTS] = {};
};
