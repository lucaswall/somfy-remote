#pragma once

#include <stdint.h>

// Just enough CC1101 to hold the radio in asynchronous OOK mode while something else works
// the data pin. No FIFO and no packet engine: RTS is a bit-banged waveform.
//
// **GDO0 is shared and only one side may drive it at a time.** §27.1 allows the demodulated
// data out on GDO0, so receive needs no second wire — but the chip drives that pin in
// receive and the ESP drives it in transmit. release() hands it over, receive() takes it
// back. Registers are the datasheet's; see docs/hardware.md.

// Status registers, for the self-test.
#define CC1101_PARTNUM 0x30
#define CC1101_VERSION 0x31
#define CC1101_RSSI 0x34
#define CC1101_MARCSTATE 0x35

// MARCSTATE values worth naming. Table for 0x35 in the datasheet.
#define CC1101_STATE_RX 0x0D
#define CC1101_STATE_TX 0x13

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
  //
  // Call release() first, then drive the data pin, then this. It restores GDO0's serial
  // data configuration on the way into transmit, so the chip sends under exactly the
  // register state it has used since 2023.
  bool transmit();

  // Enters receive with the demodulated data on GDO0. The caller must have made its own
  // pin an input first — from here the chip is driving that wire.
  bool receive();

  // IDLE, with GDO0 3-stated so the ESP can drive it. The resting state of the radio, and
  // the only state in which it is safe to key the transmitter by hand.
  void release();

  // Drives GDO0 to a fixed level — "HW to 0", and the same inverted. It exists for one
  // purpose: the ESP can then read that pin back and find out whether the wire is really
  // there. Nothing else in this driver can answer that question.
  void driveGdo0(bool high);

  void idle();

  // True when the version register reads back a value a real CC1101 returns. 0x00 means
  // MISO is stuck low or the module is unpowered; 0xFF means it is floating.
  bool present();

  uint8_t readStatus(uint8_t address);

  // Current RSSI in dBm — §17.3's conversion with this band's 74 dB offset. Only meaningful
  // in receive; in IDLE the chip holds whatever it measured last. Worth having because a
  // control that is never heard and a control that is not transmitting produce identical
  // counters, and this separates them without a frame ever having to decode.
  int16_t rssiDbm();

 private:
  uint8_t readStatusOnce(uint8_t address);
  bool waitReady();
  bool select();
  void deselect();
  void strobe(uint8_t command);
  void writeRegister(uint8_t address, uint8_t value);
  void writeBurst(uint8_t address, const uint8_t *values, uint8_t count);
  void reset();
  void configure();
  void setFrequency(float megahertz);
  bool waitForState(uint8_t want);

  uint8_t _csn;
};
