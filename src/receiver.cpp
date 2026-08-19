#include "receiver.h"

#include <Arduino.h>

#include "log.h"
#include "timing.h"

// --- the interrupt half -------------------------------------------------------------------
//
// Everything below the next divider runs in interrupt context and must live in IRAM: the
// record store programs flash on every press, and the instruction cache is off while it
// does. A handler in flash crashes only while flash happens to be busy, which is the
// hardest possible failure to reproduce.

// 256 entries is a kilobyte, and covers about 250 ms of a real press at its ~1 kHz edge
// rate — a press is five frames over 800 ms, so a stall costs frames rather than the press.
// It deliberately does not cover the longest stall this firmware has: a blocking broker
// reconnect costs seconds, and a press lost to that window had nowhere to be published
// anyway. Sizing for it would buffer events that get thrown away at the far end.
#define RING_SIZE 256

// Below this an interval is not a symbol. Somfy's shortest is 640 µs, and the datasheet
// promises 37-38.5 ns glitches on this pin in asynchronous serial mode, "occurring
// infrequently and with random periods".
//
// The rejected edge deliberately does *not* update the last-edge stamp, so a glitch is
// absorbed into the interval it interrupted rather than splitting it in two. Note what this
// is and is not: it decimates a noise storm rather than suppressing it — edges arriving
// every 200 µs are rejected twice and accepted on the third — so arbitrary noise still
// reaches the ring at up to one entry per 448 µs. The decoder is what rejects those.
#define GLITCH_US 448

// The rate limit, as a budget rather than a number picked by feel: at most 2 % of the CPU
// in this handler. One interrupt costs a shade under 4 µs, so 2 % of a 10 ms window is
// fifty of them — 5 kHz sustained. Past that the receiver mutes itself and the main loop
// backs it off, because a feature nobody is watching must not compete with the WiFi stack.
#define RATE_WINDOW_US 10000
#define EDGE_BUDGET 50

// ...but only when it stays over budget. This is the correction to a limiter that fired on
// the first press it ever heard.
//
// A real transmission is loud, and an OOK receiver fills the silence between symbols with
// whatever the AGC can find, so the interrupt rate during a press is far above the rate in
// a quiet band — measured at six times it. Muting detaches the interrupt, so a limiter that
// fires on one 10 ms window destroys the reception it exists to protect.
//
// What actually distinguishes the two is duration, not rate. A press is five frames over
// about 800 ms. A noise storm is minutes. Two seconds of continuous over-budget windows is
// comfortably longer than any press and far shorter than anything worth backing off from.
#define OVER_BUDGET_WINDOWS 200

// Presses waiting to be acted on. Four, matching the burst slots: two people pressing at
// once during the naming walk is the case that produces more than one in a single drain,
// and it is the case the assembler already sizes for.
#define PRESS_SLOTS 4

static volatile uint32_t ringEntries[RING_SIZE];
static volatile uint16_t ringHead = 0;
static uint16_t ringTail = 0;   // written only by the main loop

static volatile bool isrMuted = false;
static volatile uint32_t isrLastEdge = 0;
static volatile uint32_t isrWindowStart = 0;
static volatile uint16_t isrWindowEdges = 0;
static volatile uint16_t isrOverBudget = 0;
static volatile uint16_t isrPeakRate = 0;   // busiest window seen, in edges per 10 ms
static volatile uint32_t isrInterrupts = 0;
static volatile uint32_t isrRingWrites = 0;
static volatile uint32_t isrOverflows = 0;
static uint8_t isrPin = 0;

// Reads the GPIO input register directly rather than calling digitalRead(): one load, no
// call, and no question about whether the core put that function in IRAM.
static void IRAM_ATTR onEdge() {
  const uint32_t now = micros();
  isrInterrupts++;
  if (isrMuted) {
    return;
  }

  // The rate check counts *interrupts*, not accepted edges, and so has to come before the
  // glitch filter. A storm of sub-symbol noise is invisible past the filter and still costs
  // the whole CPU bill; counting after it would report a quiet ring while the SDK starved.
  if ((uint32_t)(now - isrWindowStart) >= RATE_WINDOW_US) {
    if (isrWindowEdges > isrPeakRate) {
      isrPeakRate = isrWindowEdges;
    }
    // Consecutive, not cumulative: one busy window in the middle of a quiet minute says
    // somebody pressed a button, and that is the opposite of a reason to stop listening.
    isrOverBudget = isrWindowEdges > EDGE_BUDGET ? (uint16_t)(isrOverBudget + 1) : 0;
    isrWindowStart = now;
    isrWindowEdges = 0;
  }
  isrWindowEdges++;
  if (isrOverBudget >= OVER_BUDGET_WINDOWS) {
    isrMuted = true;   // the main loop does the detaching; this is not the place for it
    return;
  }

  if ((uint32_t)(now - isrLastEdge) < GLITCH_US) {
    return;
  }
  isrLastEdge = now;

  // The producer writes head and the consumer writes tail, so a full ring drops the newest
  // entry rather than advancing tail from an interrupt. The stream is broken either way and
  // the decoder gives up on the frame; a data race would not be so polite.
  const uint16_t next = (uint16_t)((ringHead + 1) % RING_SIZE);
  if (next == ringTail) {
    isrOverflows++;
    return;
  }
  // The level rides in bit 0. One microsecond of resolution is nothing against a 640 µs
  // symbol, and carrying the level is what lets the decoder notice a lost edge — levels
  // alternate by construction, so a repeat means one went missing.
  ringEntries[ringHead] = (now & ~1u) | (GPIP(isrPin) ? 1u : 0u);
  ringHead = next;
  isrRingWrites++;
}

// --- the main-loop half -------------------------------------------------------------------

void Receiver::begin(uint8_t dataPin) {
  _dataPin = dataPin;
  isrPin = dataPin;
}

bool Receiver::arm(uint16_t minutes) {
  if (!_radio.ready()) {
    logError("receiver  : radio is not ready, cannot listen");
    return false;
  }

  _expiresAt = millis() + (uint32_t)minutes * 60000UL;
  if (_armed) {
    return true;   // extending the window, not restarting the radio mid-frame
  }

  _armed = true;
  _cooling = false;
  _backoffMs = 1000;
  _decoder.reset();
  // A transmission in progress is the one case where not attaching now is correct: resume()
  // does it when the radio comes back.
  if (!_suspended && !attach()) {
    _armed = false;
    return false;
  }
  logLine("receiver  : listening for %u minutes", minutes);
  return true;
}

void Receiver::disarm() {
  if (!_armed) {
    return;
  }
  _armed = false;
  _cooling = false;
  detach();
  logLine("receiver  : stopped listening");
}

uint32_t Receiver::secondsLeft() const {
  if (!_armed) {
    return 0;
  }
  const uint32_t now = millis();
  return (int32_t)(_expiresAt - now) > 0 ? (uint32_t)(_expiresAt - now) / 1000 : 0;
}

// The pin turns round in here, and the order is the whole safety argument: the chip's
// driver is 3-stated before the ESP's is switched on, and the ESP's is switched off before
// the chip's comes back. Two push-pull outputs on one wire is a short.
bool Receiver::attach() {
  if (_attached) {
    return true;
  }
  pinMode(_dataPin, INPUT);
  if (!_radio.receive()) {
    logError("receiver  : CC1101 would not enter receive");
    digitalWrite(_dataPin, LOW);
    pinMode(_dataPin, OUTPUT);
    return false;
  }

  isrMuted = false;
  isrWindowEdges = 0;
  isrOverBudget = 0;
  isrWindowStart = micros();
  isrLastEdge = isrWindowStart;
  ringTail = ringHead;
  _haveLastEntry = false;
  _decoder.reset();
  // The AGC settles for a few milliseconds after entering receive and manufactures edges
  // while it does. Ignoring them is cheaper than teaching the decoder to.
  _blankUntil = millis() + 5;
  _cleanSince = millis();

  attachInterrupt(digitalPinToInterrupt(_dataPin), onEdge, CHANGE);
  _attached = true;
  return true;
}

void Receiver::detach() {
  if (!_attached) {
    return;
  }
  detachInterrupt(digitalPinToInterrupt(_dataPin));
  _attached = false;
  _radio.release();
  digitalWrite(_dataPin, LOW);   // written before the driver is enabled, never after
  pinMode(_dataPin, OUTPUT);     // an idle high would key the transmitter continuously
}

// Suspend and resume bracket a transmission, and resume restores what suspend found: a
// receiver that was disarmed stays disarmed, and one that was cooling stays cooling.
void Receiver::suspend() {
  _suspended = true;
  detach();
}

void Receiver::resume() {
  _suspended = false;
  if (!_armed || _cooling) {
    return;
  }
  if (!attach()) {
    // A transmission that leaves the chip unable to return to receive is the radio failing,
    // not the receiver. Cool off and retry on the backoff; the main loop's own radio retry
    // will have re-run begin() by then if the chip really has gone.
    _cooling = true;
    _muteUntil = millis() + _backoffMs;
  }
}

void Receiver::loop() {
  const uint32_t now = millis();

  if (_armed && (int32_t)(now - _expiresAt) >= 0) {
    disarm();
    return;
  }
  if (!_armed || _suspended) {
    return;
  }

  enforceRateLimit(now);
  if (_attached) {
    applyEdges();
  }

  // A rate readout that a human can act on, sampled rather than computed per edge.
  if (elapsed(now, _lastRateAt, 1000)) {
    const uint32_t total = isrInterrupts;
    _edgeRate = total - _lastRateCount;
    _lastRateCount = total;
    _lastRateAt = now;
  }
}

// The flag is consumed in the same step that acts on it. Left set, this branch re-fires
// every pass — thousands of times a second — pushing the retry further away each time and
// saturating the backoff within milliseconds, and the receiver never comes back.
void Receiver::enforceRateLimit(uint32_t now) {
  if (_attached && isrMuted) {
    detach();
    isrMuted = false;
    _cooling = true;
    _mutes++;
    _muteUntil = now + _backoffMs;
    logError("receiver  : %lu edges/s, muted for %lu ms",
             (unsigned long)_edgeRate, (unsigned long)_backoffMs);
    _backoffMs = _backoffMs * 2 > 60000UL ? 60000UL : _backoffMs * 2;
    return;
  }

  if (_cooling && (int32_t)(now - _muteUntil) >= 0) {
    if (!attach()) {
      // The chip would not go back into receive. Stay in cooling and let the backoff carry
      // the retry, rather than sitting armed with nothing listening — a receiver that has
      // silently stopped receiving is indistinguishable from a quiet house, which is the
      // whole reason the counters on /status exist.
      _muteUntil = now + _backoffMs;
      _backoffMs = _backoffMs * 2 > 60000UL ? 60000UL : _backoffMs * 2;
      return;
    }
    _cooling = false;
    return;
  }

  // A clean minute pays the escalation back, so one noisy afternoon does not leave the
  // receiver on a sixty-second retry for the rest of the week.
  if (_attached && elapsed(now, _cleanSince, 60000)) {
    _backoffMs = 1000;
    _cleanSince = now;
  }
}

void Receiver::applyEdges() {
  const uint16_t head = ringHead;   // sampled once: the producer is still running
  const uint32_t now = millis();
  const bool blanking = (int32_t)(now - _blankUntil) < 0;

  while (ringTail != head) {
    const uint32_t entry = ringEntries[ringTail];
    ringTail = (uint16_t)((ringTail + 1) % RING_SIZE);
    if (blanking) {
      _haveLastEntry = false;
      continue;
    }

    // An interval needs two edges, and the second one may not have arrived yet — so the
    // boundary is carried across calls rather than left in the ring for a second read.
    if (!_haveLastEntry) {
      _lastEntry = entry;
      _haveLastEntry = true;
      continue;
    }

    // The level recorded at an edge is the level *after* it, so the interval that just
    // ended was held at the level of the entry before this one.
    const uint32_t interval = (entry & ~1u) - (_lastEntry & ~1u);
    const bool high = (_lastEntry & 1u) != 0;
    _lastEntry = entry;

    // Clamped rather than dropped: an interval longer than a frame contains is a gap, and
    // seeing where the gaps fall is half of what makes a capture readable.
    const uint16_t clamped = interval > 0x7FFF ? 0x7FFF : (uint16_t)interval;
    if (!_captureFrozen) {
      _capture[_captureHead] = (uint16_t)(clamped | (high ? 0x8000u : 0u));
      _captureHead = (uint16_t)((_captureHead + 1) % CAPTURE_SLOTS);
      if (_captureHead == 0) {
        _captureFilled = true;
      }
    }

    SomfyHeard heard;
    const bool decoded = _decoder.feed(high, interval, &heard);

    // Restart the capture where the data does. A sync burst is sixteen intervals of nothing
    // anybody needs to see, and letting it share the buffer with the frame means the frame
    // is cut off at the far end — which reads exactly like corruption and is not.
    if (!_captureFrozen && _decoder.takeFrameStart()) {
      _captureHead = 0;
      _captureFilled = false;
      _capture[_captureHead++] = (uint16_t)(clamped | (high ? 0x8000u : 0u));
    }

    // A frame that ran to full length and only then failed its checksum is the one thing on
    // this pin worth stopping to look at. Not any abandoned frame: noise finds a false sync
    // every few seconds and gives up within a handful of intervals, and a capture that
    // freezes on the first of those never sees the press it was armed for. Reading the
    // capture re-arms it.
    if (_decoder.takeChecksumFailure()) {
      _captureFrozen = true;
    }
    if (!decoded) {
      continue;
    }
    _frames++;

    // Impossible: the radio is in receive or transmit, never both, so this device cannot
    // hear itself. If it happens, something else is transmitting as us or the mode switch
    // is broken, and both are worth a fault rather than a shrug. The address is not logged:
    // /log and /errors need no password.
    bool ours = false;
    for (uint8_t i = 0; i < _remotes.count(); i++) {
      if (_remotes.addressOf(i) == heard.address) {
        logError("receiver  : heard our own remote %u on the air", i);
        _ownAddress++;
        ours = true;
        break;
      }
    }
    if (ours) {
      continue;
    }

    SomfyPress press;
    if (_assembler.feed(heard, now, &press)) {
      _presses++;
      recordSighting(press);
      const uint8_t next = (uint8_t)((_pressHead + 1) % PRESS_SLOTS);
      if (next == _pressTail) {
        _pressesDropped++;   // never seen in practice, but silence here would be a lie
      } else {
        _pending[_pressHead] = press;
        _pressHead = next;
      }
    }
  }
}

void Receiver::recordSighting(const SomfyPress &press) {
  const uint32_t now = millis();

  for (uint8_t i = 0; i < _sightingCount; i++) {
    if (_sightings[i].address == press.address) {
      _sightings[i].lastMs = now;
      _sightings[i].lastCode = press.rollingCode;
      _sightings[i].lastCommand = press.command;
      if (_sightings[i].presses < 0xFFFF) {
        _sightings[i].presses++;
      }
      return;
    }
  }

  uint8_t slot = _sightingCount;
  if (_sightingCount < SIGHTING_SLOTS) {
    _sightingCount++;
  } else {
    // Least recently heard. Whatever is evicted was not being pressed by the person
    // standing in front of the page.
    slot = 0;
    for (uint8_t i = 1; i < SIGHTING_SLOTS; i++) {
      if ((uint32_t)(now - _sightings[i].lastMs) > (uint32_t)(now - _sightings[slot].lastMs)) {
        slot = i;
      }
    }
  }
  _sightings[slot] = {press.address, now, now, 1, press.rollingCode, press.command};
}

void Receiver::forgetSighting(uint32_t address) {
  for (uint8_t i = 0; i < _sightingCount; i++) {
    if (_sightings[i].address == address) {
      _sightings[i] = _sightings[--_sightingCount];
      return;
    }
  }
}

bool Receiver::takePress(SomfyPress *out) {
  if (_pressTail == _pressHead) {
    return false;
  }
  *out = _pending[_pressTail];
  _pressTail = (uint8_t)((_pressTail + 1) % PRESS_SLOTS);
  return true;
}

Receiver::Stats Receiver::stats() const {
  Stats s;
  s.interrupts = isrInterrupts;
  s.ringWrites = isrRingWrites;
  s.overflows = isrOverflows;
  s.frames = _frames;
  s.presses = _presses;
  s.aborted = _decoder.aborted();
  s.mutes = _mutes;
  s.ownAddress = _ownAddress;
  s.pressesDropped = _pressesDropped;
  s.peakRate = isrPeakRate;
  s.levelRepeats = _decoder.levelRepeats();
  s.muted = _cooling;
  return s;
}
