#pragma once

#include <stdint.h>

#include "CC1101.h"
#include "somfy_frame.h"

// The transmitter: a CC1101 held in asynchronous OOK mode with the RTS waveform bit-banged
// onto its data pin. Nothing else touches the radio.
class SomfyRadio {
 public:
  // dataPin drives the chip's GDO0. It is the only line the waveform goes out on, and it
  // belongs to this class rather than to the driver, which never configures it.
  SomfyRadio(uint8_t csnPin, uint8_t dataPin) : _cc1101(csnPin), _dataPin(dataPin) {}

  bool begin();
  bool ready() const { return _ready; }

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
  uint8_t _dataPin;
  bool _ready = false;
};
