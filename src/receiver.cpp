#include "receiver.h"

#include <Arduino.h>

#include "log.h"
#include "timing.h"

// --- the interrupt half -------------------------------------------------------------------
//
// **Everything down to the next divider runs in interrupt context and must stay in IRAM.**
// The record store programs flash on every press and the instruction cache is off while it
// does, so a handler in flash crashes only while flash happens to be busy.

// A kilobyte, ~250 ms of a press. Deliberately not sized for this firmware's longest stall —
// a blocking broker reconnect costs seconds, and a press lost to that had nowhere to be
// published anyway.
#define RING_SIZE 256

// Below this an interval is not a symbol; the datasheet promises 37-38.5 ns glitches on this
// pin in asynchronous serial mode. A rejected edge does not update the last-edge stamp, so a
// glitch is absorbed rather than splitting the interval. This decimates a storm rather than
// suppressing it: noise still reaches the ring at up to one entry per GLITCH_US.
#define GLITCH_US 448

// At most 2 % of the CPU in this handler: ~4 µs an interrupt, so fifty per 10 ms window.
#define RATE_WINDOW_US 10000
#define EDGE_BUDGET 50

// Sustained pressure, not a burst. A press is loud — an OOK receiver fills the gaps between
// symbols with whatever the AGC finds — and muting detaches the interrupt, so a limiter that
// fires on one window destroys the reception it protects. Two seconds is far longer than the
// 800 ms a press lasts and far shorter than a noise storm.
#define OVER_BUDGET_WINDOWS 200

#define PRESS_SLOTS 4

// Five attempts across the doubling backoff is about half a minute of a chip refusing to
// receive — long enough that it is not a passing noise storm, short enough that a bridge
// does not spend the night deaf.
#define ATTACH_FAILURES_BEFORE_RESET 5

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

// Reads the GPIO register directly rather than calling digitalRead(): no question about
// whether the core put that function in IRAM.
static void IRAM_ATTR onEdge() {
  const uint32_t now = micros();
  isrInterrupts++;
  if (isrMuted) {
    return;
  }

  // Counts interrupts, not accepted edges, so it must precede the glitch filter: sub-symbol
  // noise is invisible past the filter and still costs the whole CPU bill.
  if ((uint32_t)(now - isrWindowStart) >= RATE_WINDOW_US) {
    if (isrWindowEdges > isrPeakRate) {
      isrPeakRate = isrWindowEdges;
    }
    // Leaky, not consecutive: a storm that straddles the budget would never escalate if one
    // window at or under it reset the count, while a decay still ignores a lone burst.
    if (isrWindowEdges > EDGE_BUDGET) {
      isrOverBudget++;
    } else if (isrOverBudget > 0) {
      isrOverBudget--;
    }
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

  // Producer writes head, consumer writes tail — so a full ring drops the newest rather than
  // advancing tail from an interrupt. The stream is broken either way; a race would not be.
  const uint16_t next = (uint16_t)((ringHead + 1) % RING_SIZE);
  if (next == ringTail) {
    isrOverflows++;
    return;
  }
  // Level in bit 0; a microsecond of resolution is nothing against a 640 µs symbol.
  ringEntries[ringHead] = (now & ~1u) | (GPIP(isrPin) ? 1u : 0u);
  ringHead = next;
  isrRingWrites++;
}

// --- the main-loop half -------------------------------------------------------------------

void Receiver::begin(uint8_t dataPin) {
  _dataPin = dataPin;
  isrPin = dataPin;
  _dwell.begin(millis());
}

const RxDwell &Receiver::dwell() {
  _dwell.advance(millis());
  return _dwell;
}

// **The pin turns round here, and the order is the safety argument.** The chip's driver is
// 3-stated before the ESP's is switched on, and off before the chip's comes back; two
// push-pull outputs on one wire is a short.
bool Receiver::attach() {
  if (_attached) {
    return true;
  }
  pinMode(_dataPin, INPUT);
  if (!_radio.receive()) {
    logError("receiver  : CC1101 would not enter receive");
    // receive() has already strobed SRX, so the chip drives GDO0 whatever MARCSTATE says.
    _radio.release();
    digitalWrite(_dataPin, LOW);
    pinMode(_dataPin, OUTPUT);

    // **The backoff belongs here, not at the callers.** loop() retries whenever the receiver
    // is neither attached nor cooling, so a chip that will not enter receive would otherwise
    // be retried — and logged — on every pass, thousands of times a second.
    _cooling = true;
    _dwell.set(millis(), RxDwell::Muted);
    _muteUntil = millis() + _backoffMs;
    _backoffMs = _backoffMs * 2 > 60000UL ? 60000UL : _backoffMs * 2;

    // Retrying SRX forever is what left the chip wedged for six hours: the fault was in the
    // register set, and nothing short of SRES was ever going to rewrite it. Past the
    // threshold the radio is handed back for a full reinitialisation instead. Cleared as it
    // fires so a radio that comes back and falls over again has to earn its next reset
    // rather than triggering one on every retry.
    if (++_attachFailures >= ATTACH_FAILURES_BEFORE_RESET) {
      logError("receiver  : %u failed attempts to receive, resetting the radio",
               _attachFailures);
      _radio.markFaulted();
      _attachFailures = 0;
    }
    return false;
  }

  _attachFailures = 0;

  isrMuted = false;
  isrWindowEdges = 0;
  isrOverBudget = 0;
  isrWindowStart = micros();
  isrLastEdge = isrWindowStart;
  ringTail = ringHead;
  _haveLastEntry = false;
  _decoder.reset();
  // The AGC manufactures edges for a few milliseconds after entering receive.
  _blankUntil = millis() + 5;
  _cleanSince = millis();

  attachInterrupt(digitalPinToInterrupt(_dataPin), onEdge, CHANGE);
  _attached = true;
  _dwell.set(millis(), RxDwell::Receiving);
  return true;
}

void Receiver::detach(bool captureMute) {
  if (!_attached) {
    return;
  }
  detachInterrupt(digitalPinToInterrupt(_dataPin));
  _attached = false;
  _dwell.set(millis(), _suspended ? RxDwell::Suspended : RxDwell::Muted);
  if (captureMute && !_radioSnapshot.attempted) {
    captureRadioSnapshot();
  }
  _radio.release();
  digitalWrite(_dataPin, LOW);   // before the driver is enabled: an idle high keys the PA
  pinMode(_dataPin, OUTPUT);
}

void Receiver::captureRadioSnapshot() {
  // ISR detached, chip still in its active configuration: release() changes IOCFG0.
  _radioSnapshot.attempted = true;
  _radioSnapshot.atMs = millis();
  _radioSnapshot.mute = _mutes + 1;
  _radioSnapshot.marcState = _radio.marcState();
  for (uint8_t i = 0; i < RADIO_REGISTER_COUNT; ++i) {
    uint8_t value = 0, again = 0;
    const bool ok = _radio.readConfig(RADIO_REGISTERS[i].address, &value);
    const bool okAgain = _radio.readConfig(RADIO_REGISTERS[i].address, &again);
    _radioSnapshot.add(i, ok, okAgain, value, again);
  }
  logLine("rx config : first mute, mismatch 0x%04X invalid 0x%04X",
          _radioSnapshot.mismatches, _radioSnapshot.invalid);
  if (_radioSnapshot.faulted()) {
    _radio.markFaulted();
  }
}

// Resume restores what suspend found: a receiver that was cooling stays cooling.
void Receiver::suspend() {
  _suspended = true;
  detach();
  _dwell.set(millis(), RxDwell::Suspended);
}

void Receiver::resume() {
  _suspended = false;
  _dwell.set(millis(), _cooling ? RxDwell::Muted : RxDwell::Inactive);
  if (_cooling) {
    return;
  }
  attach();   // sets its own backoff if the radio refuses
}

void Receiver::loop() {
  const uint32_t now = millis();
  _dwell.advance(now);

  if (_suspended) {
    return;
  }

  // Attached here rather than in begin(), and only once the chip is configured: SRES leaves
  // a 135 kHz divided crystal clock on GDO0 until configure() runs, which is a hundred
  // thousand interrupts a second straight into the handler. This also re-attaches by itself
  // after a radio recovery.
  if (!_attached && !_cooling && _radio.ready()) {
    attach();
  }

  enforceRateLimit(now);
  if (_attached) {
    applyEdges();
  }

  if (_attached) {
    sampleRssi(now);
  }

  if (elapsed(now, _lastRateAt, 1000)) {
    const uint32_t total = isrInterrupts;
    _edgeRate = total - _lastRateCount;
    _lastRateCount = total;
    _lastRateAt = now;

    // **A chip that has fallen out of receive looks exactly like a quiet house.** The
    // interrupt is still attached, the pin simply stops moving, and every counter freezes
    // at the value it had — so nothing above this line can tell the difference. On
    // 2026-08-20 the radio sat in RXFIFO_OVERFLOW for six hours while /status reported
    // "listening" and the presses that should have closed the gallery were logged as sent.
    //
    // Recovered through the same backoff a noise storm uses, so a chip that will not stay
    // in receive cannot spin here: attach() re-enters RX with the FIFO flushed, and gives
    // up for progressively longer if it keeps failing.
    if (_attached) {
      const uint8_t state = _radio.marcState();
      if (state != CC1101_STATE_RX) {
        logError("receiver  : chip left receive (marcstate 0x%02X), restarting it", state);
        detach();
        _cooling = true;
        _muteUntil = now + _backoffMs;
        _backoffMs = _backoffMs * 2 > 60000UL ? 60000UL : _backoffMs * 2;
      }
    }
  }
}

// **The mute flag is consumed in the same step that acts on it.** Left set, this branch
// re-fires every pass, pushing the retry further away each time until the receiver is dead.
// A ten-millisecond cadence catches a Somfy burst — the frame alone runs tens of
// milliseconds — while costing a twenty-microsecond SPI read a hundred times a second.
// The peak decays over five seconds so it reads the same however often anyone polls it.
void Receiver::sampleRssi(uint32_t now) {
  if (!elapsed(now, _lastRssiAt, 10)) {
    return;
  }
  _lastRssiAt = now;
  _rssiNow = _radio.rssiDbm();
  if (_rssiNow > _rssiPeak || (uint32_t)(now - _rssiPeakAt) > 5000) {
    _rssiPeak = _rssiNow;
    _rssiPeakAt = now;
  }
}

void Receiver::enforceRateLimit(uint32_t now) {
  if (_attached && isrMuted) {
    detach(true);
    isrMuted = false;
    _cooling = true;
    _mutes++;
    _muteUntil = now + _backoffMs;
    logError("receiver  : %lu edges/s, muted for %lu ms",
             (unsigned long)_edgeRate, (unsigned long)_backoffMs);
    _backoffMs = _backoffMs * 2 > 60000UL ? 60000UL : _backoffMs * 2;
    return;
  }

  // Stay cooling rather than sit with nothing listening: a receiver that has silently
  // stopped is indistinguishable from a quiet house. attach() re-arms its own backoff.
  if (_cooling && (int32_t)(now - _muteUntil) >= 0) {
    if (attach()) {
      _cooling = false;
    }
    return;
  }

  // A clean minute pays the escalation back.
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

    // An interval needs two edges and the second may not have arrived, so the boundary is
    // carried across calls rather than left in the ring.
    if (!_haveLastEntry) {
      _lastEntry = entry;
      _haveLastEntry = true;
      continue;
    }

    // An edge records the level *after* it, so the interval that just ended was held at the
    // level of the entry before this one.
    const uint32_t interval = (entry & ~1u) - (_lastEntry & ~1u);
    const bool high = (_lastEntry & 1u) != 0;
    _lastEntry = entry;

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
    // Consumed unconditionally, acted on conditionally: latched through a freeze it would
    // fire on whatever arrived after a read and present it as a frame boundary.
    const bool frameStarted = _decoder.takeFrameStart();
    if (frameStarted && !_captureFrozen) {
      _captureHead = 0;
      _captureFilled = false;
      _capture[_captureHead++] = (uint16_t)(clamped | (high ? 0x8000u : 0u));
    }

    if (_decoder.takeChecksumFailure()) {
      _captureFrozen = true;
    }

    if (!decoded) {
      continue;
    }
    _frames++;

    // Impossible — the radio is in one mode at a time, so this device cannot hear itself.
    // If it happens, something else is transmitting as us. The address is not logged:
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
      // Oldest goes, unlike the ISR ring: both ends run in the main loop here, and the press
      // that says where a shutter came to rest is the last one.
      const uint8_t next = (uint8_t)((_pressHead + 1) % PRESS_SLOTS);
      if (next == _pressTail) {
        _pressTail = (uint8_t)((_pressTail + 1) % PRESS_SLOTS);
        _pressesDropped++;
      }
      _pending[_pressHead] = press;
      _pressHead = next;
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

  if (!discovering()) {
    _ignored++;   // heard, understood, and not ours to care about
    return;
  }
  _discoverUntil = 0;   // one press per window: the next one is a different decision

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

void Receiver::discover(uint16_t seconds) {
  _discoverUntil = millis() + (uint32_t)seconds * 1000;
  logLine("receiver  : learning a new control for %us", seconds);
}

bool Receiver::discovering() const {
  return _discoverUntil != 0 && (int32_t)(millis() - _discoverUntil) < 0;
}

uint32_t Receiver::discoverSecondsLeft() const {
  return discovering() ? (uint32_t)(_discoverUntil - millis()) / 1000 : 0;
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

Receiver::Stats Receiver::stats() {
  Stats s;
  s.interrupts = isrInterrupts;
  s.ringWrites = isrRingWrites;
  s.overflows = isrOverflows;
  s.frames = _frames;
  s.presses = _presses;
  s.abandoned = _decoder.abandoned();
  s.badChecksum = _decoder.badChecksum();
  s.mutes = _mutes;
  s.ownAddress = _ownAddress;
  s.pressesDropped = _pressesDropped;
  s.ignored = _ignored;
  s.peakRate = isrPeakRate;
  s.levelRepeats = _decoder.levelRepeats();
  s.marcState = _radio.marcState();
  s.rssiNow = _rssiNow;
  s.rssiPeak = _rssiPeak;
  s.muted = _cooling;
  return s;
}
