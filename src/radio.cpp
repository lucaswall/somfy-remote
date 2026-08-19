#include "radio.h"

#include <Arduino.h>

#include "log.h"
#include "somfy_pulses.h"

// Somfy RTS. Not 433.92, which is where nearly every other 433 MHz device sits and where
// a Somfy motor hears nothing.
static const float SOMFY_MHZ = 433.42f;

bool SomfyRadio::begin() {
  // Left as an input across begin(): SRES restores IOCFG0 to its default, which is a
  // divided crystal clock driven out of GDO0, and the corrective write comes several
  // SPI transactions later. Driving the pin before then puts two push-pull outputs on
  // one wire for the length of the reset.
  pinMode(_dataPin, INPUT);

  _ready = _cc1101.begin(SOMFY_MHZ);
  if (_ready) {
    pinMode(_dataPin, OUTPUT);
    digitalWrite(_dataPin, LOW);   // an idle high would key the transmitter continuously
  }
  return _ready;
}

bool SomfyRadio::send(SomfyCommand command, uint32_t address, uint16_t rollingCode) {
  if (!_ready) {
    return false;
  }
  if (!_cc1101.transmit()) {
    // Nothing left the antenna, and the chip is no longer answering as expected. Drop
    // back to unready so the main loop re-runs begin() rather than transmitting into a
    // radio that has stopped listening to us.
    logError("radio     : chip would not enter transmit, reinitialising");
    _ready = false;
    return false;
  }

  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(command, rollingCode, address, frame);

  // The wake-up burst. A receiver spends most of its time asleep and samples the band
  // briefly; this is the pulse long enough that it cannot be missed.
  digitalWrite(_dataPin, HIGH);
  delayMicroseconds(SOMFY_WAKEUP_HIGH_US);
  digitalWrite(_dataPin, LOW);
  delayMicroseconds(SOMFY_WAKEUP_LOW_US);
  delay(SOMFY_WAKEUP_GAP_MS);

  playFrame(frame, SOMFY_FIRST_SYNC);
  delay(SOMFY_INTERFRAME_GAP_MS);
  for (uint8_t i = 0; i < SOMFY_REPEATS; i++) {
    playFrame(frame, SOMFY_REPEAT_SYNC);
    delay(SOMFY_INTERFRAME_GAP_MS);
  }

  _cc1101.idle();
  digitalWrite(_dataPin, LOW);
  return true;
}

// Interrupts stay enabled, unlike every reference implementation of this protocol.
//
// A frame is 113 ms of pulses. Blocking the SDK's interrupts for that long on a chip that
// is also running a WiFi stack is how an RTS bridge ends up dropping its association or
// resetting under load. The cost of leaving them on is that an interrupt can stretch a
// pulse by tens of microseconds — absorbed here by timing each edge against an absolute
// deadline, so a late edge steals from its own pulse instead of shifting every one after
// it. RTS receivers tolerate far more jitter than that; they do not tolerate a bridge that
// reboots.
void SomfyRadio::playFrame(const uint8_t *frame, uint8_t syncCount) {
  SomfyPulse pulses[SOMFY_MAX_PULSES];
  const size_t count = somfyBuildPulses(frame, syncCount, pulses, SOMFY_MAX_PULSES);
  if (count == 0) {
    logError("radio     : %u sync pulses do not fit the waveform buffer", syncCount);
    return;
  }

  uint32_t deadline = micros();
  for (size_t i = 0; i < count; i++) {
    digitalWrite(_dataPin, pulses[i].high ? HIGH : LOW);
    deadline += pulses[i].microseconds;
    while ((int32_t)(micros() - deadline) < 0) {
    }
  }
  digitalWrite(_dataPin, LOW);
}
