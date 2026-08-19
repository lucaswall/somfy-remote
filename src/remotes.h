#pragma once

#include <stdint.h>

#include "command_queue.h"
#include "radio.h"
#include "remote_state.h"
#include "rolling_code.h"

// The emulated remotes: one per shutter, each with its own address, its own rolling code
// counter in EEPROM, and the state we have inferred from what we sent it.
//
// This is the layer that knows nothing about MQTT or HTTP. Both of those queue commands
// here and read state back from here, so a press in Home Assistant and a press on the web
// page follow exactly the same path.
class Remotes {
 public:
  // Addresses run consecutively from addressBase. count is clamped to the number of
  // rolling code slots EEPROM has room for.
  Remotes(SomfyRadio &radio, uint32_t addressBase, uint8_t count);

  void begin();

  // Queued rather than sent: a press takes most of a second on the air, and the caller is
  // usually an MQTT callback. An unknown remote is logged as a fault and dropped here.
  void queue(uint8_t remote, SomfyCommand command);

  // Sends at most one queued command, so the loop keeps serving MQTT and OTA between
  // presses.
  void loop();

  uint8_t count() const { return _count; }
  uint8_t pending() const { return _queue.count(); }
  const RemoteState &state(uint8_t remote) const { return _states[remote]; }

  // The code the next press will use. Diagnostic only: a counter that has stopped moving
  // is the signature of an EEPROM that is no longer being written.
  uint16_t rollingCode(uint8_t remote) const;

 private:
  uint16_t takeRollingCode(uint8_t remote);

  SomfyRadio &_radio;
  uint32_t _base;
  uint8_t _count;
  CommandQueue _queue;
  RemoteState _states[ROLLING_CODE_MAX_REMOTES];
};
