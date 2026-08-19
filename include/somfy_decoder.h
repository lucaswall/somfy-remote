#pragma once

#include <stdint.h>
#include <string.h>

#include "somfy_frame.h"
#include "somfy_pulses.h"

// Edge intervals back into presses.
//
// **Runs in the main loop, never in the interrupt handler.** Reference implementations of
// this protocol decode inside the ISR; this chip shares its core with a WiFi stack.

// Wide because somfy_pulses.h's timings came from other people's remotes, not a datasheet.
#define SOMFY_TOLERANCE_PERCENT 30

// Pairs, as intervals. Two rather than SOMFY_REPEAT_SYNC's seven: only the repeats carry
// seven, so demanding them would discard the first frame of every press.
#define SOMFY_MIN_SYNC_INTERVALS (2 * SOMFY_FIRST_SYNC)

// Ends whatever was in progress. Boxed in on both sides: above the software sync at its
// slowest tolerated (5915 µs), below the 9415 µs wake-up pulse this most needs to reset on.
#define SOMFY_GAP_US 7000

inline bool somfyNear(uint32_t measured, uint32_t expected) {
  const uint32_t slack = expected * SOMFY_TOLERANCE_PERCENT / 100;
  return measured + slack >= expected && measured <= expected + slack;
}

// Fed (level, duration) pairs; reports a frame once 56 bits arrive and the checksum holds.
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
    // Counted, never acted on — see feedData().
    if (_haveLevel && high == _lastHigh) {
      _levelRepeats++;
    }
    _haveLevel = true;
    _lastHigh = high;

    if (microseconds > SOMFY_GAP_US) {
      abandon();
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

  // Separate because they call for opposite fixes: a broken interval train versus timing
  // that held while the bits came out wrong. 32-bit — 16 wraps in under two minutes here.
  uint32_t abandoned() const { return _abandoned; }
  uint32_t badChecksum() const { return _badChecksum; }

  // Consecutive intervals at the same level, which cannot happen if every edge was seen —
  // so the sharpest available measure of how much the front end is dropping.
  uint32_t levelRepeats() const { return _levelRepeats; }

  // Lets a diagnostic capture start where the data does; a sync burst otherwise fills the
  // buffer and the frame is clipped at the far end, which reads as corruption and is not.
  bool takeFrameStart() {
    const bool started = _frameStarted;
    _frameStarted = false;
    return started;
  }

  // Not "any abandoned frame": noise finds a false sync every few seconds and gives up
  // within a handful of intervals, so a capture armed on those never sees a real press.
  bool takeChecksumFailure() {
    const bool failed = _checksumFailed;
    _checksumFailed = false;
    return failed;
  }

 private:
  void abandon() {
    if (_inData) {
      _abandoned++;
    }
    reset();
  }

  // Hardware sync first: at ±30 % the two windows overlap between 3185 and 3328 µs, and
  // mistaking a software sync for a hardware one costs this frame, where the reverse would
  // start reading data from the middle of a sync burst.
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

  // Manchester, inverted: a 1 is low then high. Bits come from the durations alone, toggled
  // from a seed at the software sync.
  //
  // **Do not derive the bit from the interval's level instead.** It is equivalent on a clean
  // stream and looks safer, but the glitch filter ahead of this drops an arbitrary number of
  // edges under a noisy line rather than a tidy pair, so the recorded level is close to
  // random and nothing decodes at all. levelRepeats() is what measures that.
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
      _badChecksum++;
      _checksumFailed = true;
    }
    reset();
    return valid;
  }

  uint8_t _frame[SOMFY_FRAME_LEN] = {0};
  uint32_t _abandoned = 0;
  uint32_t _badChecksum = 0;
  uint32_t _levelRepeats = 0;
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

// Copies are collapsed by rolling code rather than by a timer: the code *is* the press
// identifier. A pure time window would merge two quick presses and split one slow burst.
#define SOMFY_BURST_MS 1200

// The frame checksum is four bits and cannot see a single-bit error at all — test_somfy_frame
// pins it: 44 of 56 flips decode clean. One frame is not evidence of anything.
#define SOMFY_BURST_COPIES 2

// One slot is not enough: a second transmitter mid-burst would thrash it and neither press
// would reach its second copy.
#define SOMFY_BURST_SLOTS 4

struct SomfyPress {
  uint32_t address;
  uint16_t rollingCode;
  uint8_t command;
};

class SomfyPressAssembler {
 public:
  // True once per press, on the copy that confirms it. Unsigned arithmetic throughout, so
  // the millis() rollover is not a special case.
  bool feed(const SomfyHeard &heard, uint32_t nowMs, SomfyPress *out) {
    Slot *slot = find(heard.address);
    if (slot == nullptr) {
      slot = evictOldest(nowMs);
    }

    // The command must be compared too: a merged edge at the start of byte 1 inverts the
    // command nibble while address and rolling code survive, because the checksum nibble
    // moves by the same amount and cancels.
    const bool sameBurst = slot->used && slot->address == heard.address &&
                           slot->rollingCode == heard.rollingCode &&
                           slot->command == heard.command &&
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
