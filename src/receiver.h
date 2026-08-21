#pragma once

#include <stdint.h>

#include "radio.h"
#include "remotes.h"
#include "somfy_decoder.h"

// Hearing the handhelds already in the house, so a shutter opened by hand stops reading
// closed in Home Assistant.
//
// Always listening. An OOK receiver whose AGC is misconfigured amplifies noise until the
// data pin chatters, which is a real way to starve the SDK on a chip that also runs WiFi —
// the rate limiter is the guard against that, not a human remembering to switch it on.

// Forty, because a naming walk may press every control in the house before naming any of
// them, and evicting the start of that walk loses rows nobody knows are missing.
#define SIGHTING_SLOTS 40

// The last intervals the decoder was fed, circular — a one-shot capture would have to be
// started before the thing worth capturing, which nobody can time from a web page. It exists
// because a decoder rejecting everything and a radio hearing nothing produce identical
// counters. Two bytes each, level in the top bit.
#define CAPTURE_SLOTS 192

// What the bridge has heard from one address it does not know yet.
struct Sighting {
  uint32_t address;
  uint32_t firstMs;
  uint32_t lastMs;
  uint16_t presses;
  uint16_t lastCode;
  uint8_t lastCommand;
};

class Receiver {
 public:
  Receiver(SomfyRadio &radio, Remotes &remotes) : _radio(radio), _remotes(remotes) {}

  // Records which wire to watch. Listening starts from loop(), once the radio is configured.
  void begin(uint8_t dataPin);

  // False while the rate limiter is backing off after a noise storm.
  bool listening() const { return _attached; }

  // A control the bridge has never heard is only added while this window is open, so a
  // neighbour's remote pressed at the wrong moment does not join the list. It closes on the
  // first new address, or on the timeout. Presses from addresses already on the list are
  // never gated.
  void discover(uint16_t seconds);
  bool discovering() const;
  uint32_t discoverSecondsLeft() const;

  void loop();

  // Bracket a transmission: the radio cannot do both at once, and only one side may drive
  // the data pin. Resume restores what suspend found.
  void suspend();
  void resume();

  bool takePress(SomfyPress *out);

  // Oldest first. Frozen when a frame reaches full length and fails its checksum.
  uint16_t captureCount() const { return _captureFilled ? CAPTURE_SLOTS : _captureHead; }
  uint16_t captureAt(uint16_t i) const {
    return _capture[_captureFilled ? (uint16_t)((_captureHead + i) % CAPTURE_SLOTS) : i];
  }
  bool captureFrozen() const { return _captureFrozen; }
  void rearmCapture() {
    _captureHead = 0;
    _captureFilled = false;
    _captureFrozen = false;
  }

  uint8_t sightingCount() const { return _sightingCount; }
  const Sighting &sighting(uint8_t i) const { return _sightings[i]; }
  void forgetSighting(uint32_t address);

  // Not optional: a receiver that has quietly muted itself looks exactly like a quiet house.
  struct Stats {
    uint32_t interrupts;    // what the CPU actually paid for
    uint32_t ringWrites;    // what survived the glitch filter
    uint32_t overflows;
    uint32_t frames;
    uint32_t presses;
    uint32_t abandoned;    // the interval train broke: noise, a lost edge, a gap
    uint32_t badChecksum;  // the timing held and the bits were wrong
    uint16_t mutes;
    uint16_t ownAddress;    // frames carrying one of our own addresses: impossible, so a fault
    uint16_t pressesDropped;
    uint16_t ignored;        // presses from addresses nobody has asked to learn
    uint16_t peakRate;       // busiest 10 ms window ever seen, in edges
    uint32_t levelRepeats;   // how badly the front end is dropping edges
    uint8_t marcState;       // read live from the chip: 0x0D is RX
    int16_t rssiNow;         // dBm, sampled: the noise floor when nothing is transmitting
    int16_t rssiPeak;        // strongest dBm in the last few seconds — a press should spike it
    bool muted;
  };
  Stats stats();
  uint32_t edgesPerSecond() const { return _edgeRate; }

 private:
  void applyEdges();
  void recordSighting(const SomfyPress &press);
  void enforceRateLimit(uint32_t now);
  bool attach();
  void detach();

  // Sampled on a timer rather than tied to a decode: the case worth measuring is the press
  // that never becomes a frame, and there is nothing to hang a reading on then.
  void sampleRssi(uint32_t now);
  int16_t _rssiNow = -128;
  int16_t _rssiPeak = -128;
  uint32_t _rssiPeakAt = 0;
  uint32_t _lastRssiAt = 0;

  SomfyRadio &_radio;
  Remotes &_remotes;
  SomfyDecoder _decoder;
  SomfyPressAssembler _assembler;

  Sighting _sightings[SIGHTING_SLOTS] = {};
  uint8_t _sightingCount = 0;

  SomfyPress _pending[4] = {};
  uint8_t _pressHead = 0;
  uint8_t _pressTail = 0;
  uint16_t _pressesDropped = 0;

  uint16_t _capture[CAPTURE_SLOTS] = {};
  uint16_t _captureHead = 0;
  bool _captureFilled = false;
  bool _captureFrozen = false;

  uint32_t _lastEntry = 0;
  bool _haveLastEntry = false;
  uint8_t _dataPin = 0;
  bool _attached = false;
  bool _cooling = false;
  bool _suspended = false;
  uint32_t _blankUntil = 0;
  uint32_t _discoverUntil = 0;
  uint16_t _ignored = 0;
  uint32_t _muteUntil = 0;
  uint32_t _backoffMs = 1000;
  uint8_t _attachFailures = 0;
  uint32_t _cleanSince = 0;
  uint32_t _lastRateAt = 0;
  uint32_t _lastRateCount = 0;
  uint32_t _edgeRate = 0;
  uint32_t _frames = 0;
  uint32_t _presses = 0;
  uint16_t _mutes = 0;
  uint16_t _ownAddress = 0;
};
