#pragma once

#include <stdint.h>

#include "CC1101.h"
#include "somfy_frame.h"

class Receiver;

// A CC1101 in asynchronous OOK mode: the RTS waveform bit-banged onto its data pin to
// transmit, the demodulated data read back off the same pin to receive.
//
// **One wire, two directions, never both at once.** Every transmission stops the receiver
// first and restarts it on every exit path, including the one where the chip refuses TX.
class SomfyRadio {
 public:
  // dataPin drives the chip's GDO0. It is the only line the waveform goes out on, and it
  // belongs to this class rather than to the driver, which never configures it.
  SomfyRadio(uint8_t csnPin, uint8_t dataPin) : _cc1101(csnPin), _dataPin(dataPin) {}

  bool begin();
  bool ready() const { return _ready; }

  // Told rather than asked, so no caller can forget: a command arriving while the chip is
  // listening would put two push-pull drivers on one wire.
  void listener(Receiver *receiver) { _receiver = receiver; }

  // Proves the GDO0 wire is connected, with no RF involved: the chip drives it to a known
  // level both ways round and the ESP reads it back. Worth having because transmit fails
  // visibly — a shutter does not move — while receive on a broken wire looks like silence.
  bool testDataPin();

  // Here rather than on the driver, which has no idea the pin is shared.
  bool receive() { return _cc1101.receive(); }
  void release() { _cc1101.release(); }

  // Plays one press: the wake-up burst, a frame, and the repeats a receiver needs to hear
  // it. Blocks for about eight hundred milliseconds, which is why commands are queued
  // rather than sent from wherever they arrive.
  //
  // False means the radio would not enter transmit, so the caller must not record the
  // command as sent — the shutter certainly did not hear it.
  bool send(SomfyCommand command, uint32_t address, uint16_t rollingCode);

 private:
  void playFrame(const uint8_t *frame, uint8_t syncCount);

  CC1101 _cc1101;
  Receiver *_receiver = nullptr;
  uint8_t _dataPin;
  bool _ready = false;
};
