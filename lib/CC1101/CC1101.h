#pragma once

#include <stdint.h>

// Just enough CC1101 to hold the radio in asynchronous OOK transmit mode while something
// else drives the data pin. No FIFO, no packet engine, no receive path: Somfy RTS is a
// bit-banged waveform and one-way, so everything the chip could do for us is unused.
//
// Registers and values are the TI CC1101 datasheet's, and the configuration is the one the
// deployed bridge transmits with — see docs/hardware.md.

// Status registers, for the self-test.
#define CC1101_PARTNUM 0x30
#define CC1101_VERSION 0x31
#define CC1101_MARCSTATE 0x35

class CC1101 {
 public:
  // csnPin is the only line not fixed by hardware SPI. GDO0 belongs to the transmitter,
  // not to this class, which never touches it.
  explicit CC1101(uint8_t csnPin) : _csn(csnPin) {}

  // Resets the chip, writes the configuration and sets the frequency. False means the
  // chip did not answer, which is wiring or power and never software.
  bool begin(float megahertz);

  // Enters transmit and waits for the synthesiser to settle. Data sent before it does is
  // simply not transmitted, which is why this waits rather than returning immediately.
  bool transmit();

  void idle();

  // True when the version register reads back a value a real CC1101 returns. 0x00 means
  // MISO is stuck low or the module is unpowered; 0xFF means it is floating.
  bool present();

  uint8_t readStatus(uint8_t address);

 private:
  bool waitReady();
  bool select();
  void deselect();
  void strobe(uint8_t command);
  void writeRegister(uint8_t address, uint8_t value);
  void writeBurst(uint8_t address, const uint8_t *values, uint8_t count);
  void reset();
  void configure();
  void setFrequency(float megahertz);

  uint8_t _csn;
};
