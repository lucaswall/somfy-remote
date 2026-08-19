#pragma once

#include <stdint.h>

#include "control_map.h"
#include "radio.h"
#include "remotes.h"
#include "somfy_decoder.h"

// Hearing the handhelds already in the house.
//
// Everything Home Assistant shows about a cover is inferred from what this bridge believes
// it sent. Somebody using a wall button is invisible, and the covers stay confidently wrong
// until the next command from us. This is the half that listens.
//
// **It is inert until armed.** RTS shares 433 MHz with every doorbell and weather station
// in the street, and an OOK receiver with no signal amplifies noise until the data line
// toggles continuously — on a chip that is also running a WiFi stack, an unbounded edge
// interrupt is a plausible way to starve the SDK. Arming is deliberate, bounded and
// self-expiring, so a bridge nobody is teaching behaves exactly as it did before this
// existed. Whether it can safely be left on for ever is a measurement, not an opinion.
//
// The interrupt handler timestamps edges into a ring and does nothing else. Decoding runs
// in the main loop. Every reference implementation of this protocol decodes inside the
// interrupt; they are not sharing a core with an SDK that drops its WiFi association when
// starved.

// Unknown addresses waiting to be named. Forty because the naming walk may press every
// control in the house before naming any of them — a dozen wall buttons and a couple of
// multi-channel handhelds — and evicting the start of that walk would produce a list
// missing rows nobody could know were missing. Twenty bytes each.
#define SIGHTING_SLOTS 40

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

  // The data pin the radio shares between transmit and receive. Nothing is armed by this;
  // it only records which wire to watch.
  void begin(uint8_t dataPin);

  // Listen for `minutes`, then stop by itself. Arming again while armed extends the window
  // rather than restarting anything, and any heard press extends it too — during a walk a
  // press is the signal that somebody is still working, and the phoneless version of that
  // walk produces no other.
  bool arm(uint16_t minutes);
  void disarm();

  bool armed() const { return _armed; }
  uint32_t secondsLeft() const;

  void loop();

  // Called around a transmission. The radio cannot do both at once, and while the ESP is
  // driving the data pin the chip must not be. Resume restores what suspend found: a
  // disarmed receiver stays disarmed.
  void suspend();
  void resume();

  // Presses from controls this bridge has heard. Drained by whoever knows what to do with
  // them, which is not this class.
  bool takePress(SomfyPress *out);

  uint8_t sightingCount() const { return _sightingCount; }
  const Sighting &sighting(uint8_t i) const { return _sightings[i]; }
  void forgetSighting(uint32_t address);

  // Diagnostics, and not optional ones. A receiver that has quietly muted itself looks
  // exactly like a quiet house, which is the same failure the rolling code sensor exists
  // to make visible.
  struct Stats {
    uint32_t interrupts;    // what the CPU actually paid for
    uint32_t ringWrites;    // what survived the glitch filter
    uint32_t overflows;
    uint32_t frames;
    uint32_t presses;
    uint32_t aborted;
    uint16_t mutes;
    uint16_t ownAddress;    // frames carrying one of our own addresses: impossible, so a fault
    bool muted;
  };
  Stats stats() const;
  uint32_t edgesPerSecond() const { return _edgeRate; }

 private:
  void applyEdges();
  void recordSighting(const SomfyPress &press);
  void enforceRateLimit(uint32_t now);
  bool attach();
  void detach();

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

  uint32_t _lastEntry = 0;
  bool _haveLastEntry = false;
  uint8_t _dataPin = 0;
  bool _armed = false;
  bool _attached = false;
  bool _cooling = false;
  bool _suspended = false;
  uint32_t _expiresAt = 0;
  uint32_t _blankUntil = 0;
  uint32_t _muteUntil = 0;
  uint32_t _backoffMs = 1000;
  uint32_t _cleanSince = 0;
  uint32_t _lastRateAt = 0;
  uint32_t _lastRateCount = 0;
  uint32_t _edgeRate = 0;
  uint32_t _frames = 0;
  uint32_t _presses = 0;
  uint16_t _mutes = 0;
  uint16_t _ownAddress = 0;
};
