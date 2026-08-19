#include "radio.h"

#include <Arduino.h>

#include "log.h"
#include "receiver.h"
#include "somfy_pulses.h"

// Somfy RTS. Not 433.92, which is where nearly every other 433 MHz device sits and where
// a Somfy motor hears nothing.
static const float SOMFY_MHZ = 433.42f;

// Scope-based, because send() has more than one exit and a receiver left suspended is a
// receiver that never comes back. The main loop re-runs begin() every thirty seconds while
// the radio is unready, and that path leaves through the middle of the function.
namespace {
struct ResumeReceiver {
  Receiver *receiver;
  ~ResumeReceiver() {
    if (receiver != nullptr) {
      receiver->resume();
    }
  }
};
}   // namespace

bool SomfyRadio::begin() {
  // The receiver is stopped for the whole of this, not just for the SPI: SRES restores
  // IOCFG0 to its reset function, a 135 kHz divided crystal clock driven out of GDO0, and
  // the corrective write comes several transactions later. Left attached, a radio recovery
  // would feed a quarter of a million edges a second into the interrupt handler and trip
  // the rate limiter every time.
  ResumeReceiver resume{_receiver};
  if (_receiver != nullptr) {
    _receiver->suspend();
  }

  // Left as an input across begin(), for the same reason: nothing may drive that wire
  // while the chip's own driver is on it.
  pinMode(_dataPin, INPUT);

  _ready = _cc1101.begin(SOMFY_MHZ);
  if (_ready) {
    // The driver leaves GDO0 3-stated, so this is now the only driver on the wire.
    digitalWrite(_dataPin, LOW);   // an idle high would key the transmitter continuously
    pinMode(_dataPin, OUTPUT);
  }
  return _ready;
}

bool SomfyRadio::testDataPin() {
  if (!_ready) {
    return false;
  }

  pinMode(_dataPin, INPUT);
  _cc1101.driveGdo0(false);
  delayMicroseconds(50);   // a GDO output settles in nanoseconds; this is for the dupont
  const bool readLow = digitalRead(_dataPin) != 0;
  _cc1101.driveGdo0(true);
  delayMicroseconds(50);
  const bool readHigh = digitalRead(_dataPin) != 0;

  _cc1101.release();
  digitalWrite(_dataPin, LOW);
  pinMode(_dataPin, OUTPUT);
  return !readLow && readHigh;
}

bool SomfyRadio::send(SomfyCommand command, uint32_t address, uint16_t rollingCode) {
  if (!_ready) {
    return false;
  }

  // Suspended before anything else and resumed however this returns. The receiver hands the
  // pin back as it goes, so from here the ESP is the only thing driving it.
  ResumeReceiver resume{_receiver};
  if (_receiver != nullptr) {
    _receiver->suspend();
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

  _cc1101.release();
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
