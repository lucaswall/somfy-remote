#include "CC1101.h"

#include <Arduino.h>
#include <SPI.h>

// 4 MHz is inside the CC1101's 6.5 MHz single-access limit with margin for dupont wire.
static const SPISettings SPI_SETTINGS(4000000, MSBFIRST, SPI_MODE0);

// Header bits.
static const uint8_t WRITE_BURST = 0x40;
static const uint8_t READ_BURST = 0xC0;

// Strobes.
static const uint8_t SRES = 0x30;
static const uint8_t STX = 0x35;
static const uint8_t SIDLE = 0x36;

// Configuration registers used below.
static const uint8_t REG_IOCFG0 = 0x02;
static const uint8_t REG_PKTCTRL0 = 0x08;
static const uint8_t REG_FSCTRL1 = 0x0B;
static const uint8_t REG_FSCTRL0 = 0x0C;
static const uint8_t REG_FREQ2 = 0x0D;
static const uint8_t REG_MDMCFG4 = 0x10;
static const uint8_t REG_MDMCFG3 = 0x11;
static const uint8_t REG_MDMCFG2 = 0x12;
static const uint8_t REG_MDMCFG1 = 0x13;
static const uint8_t REG_MDMCFG0 = 0x14;
static const uint8_t REG_MCSM0 = 0x18;
static const uint8_t REG_FREND1 = 0x21;
static const uint8_t REG_FREND0 = 0x22;
static const uint8_t REG_FSCAL3 = 0x23;
static const uint8_t REG_FSCAL2 = 0x24;
static const uint8_t REG_FSCAL1 = 0x25;
static const uint8_t REG_FSCAL0 = 0x26;
static const uint8_t REG_FSTEST = 0x29;
static const uint8_t REG_TEST2 = 0x2C;
static const uint8_t REG_TEST1 = 0x2D;
static const uint8_t REG_TEST0 = 0x2E;
static const uint8_t REG_PATABLE = 0x3E;

// The module's crystal. Both the frequency word and every timing in the datasheet are
// derived from it; 27 MHz parts exist and would land 4 % off frequency.
static const float CRYSTAL_MHZ = 26.0f;

// MARCSTATE reads 0x13 once the chip is actually transmitting.
static const uint8_t MARCSTATE_TX = 0x13;
static const uint32_t TX_TIMEOUT_US = 10000;

// The chip pulls MISO low when it is ready to accept a header byte. Waiting on that is
// the datasheet's handshake; skipping it drops bytes on the first access after a reset.
static const uint32_t READY_TIMEOUT_US = 5000;

bool CC1101::begin(float megahertz) {
  // Driven high before SPI starts: this is GPIO15 on this board, a boot-strap pin that
  // must be low at reset, so nothing may drive it until the sketch is running.
  pinMode(_csn, OUTPUT);
  digitalWrite(_csn, HIGH);
  SPI.begin();

  reset();
  if (!present()) {
    return false;
  }
  configure();
  setFrequency(megahertz);
  idle();
  return true;
}

bool CC1101::present() {
  const uint8_t version = readStatus(CC1101_VERSION);
  return version != 0x00 && version != 0xFF;
}

bool CC1101::transmit() {
  strobe(SIDLE);
  strobe(STX);

  // Entering TX from IDLE runs a full calibration, around 720 µs. Returning before it
  // finishes would put the first sync pulse on the air with the synthesiser still moving.
  const uint32_t start = micros();
  while ((uint32_t)(micros() - start) < TX_TIMEOUT_US) {
    if (readStatus(CC1101_MARCSTATE) == MARCSTATE_TX) {
      return true;
    }
  }
  return false;
}

void CC1101::idle() { strobe(SIDLE); }

// The manual power-on reset from the datasheet's §19.1: strobe CSN, let the chip settle,
// then SRES while it is selected — and hold it selected until the chip says it is done.
void CC1101::reset() {
  digitalWrite(_csn, LOW);
  delayMicroseconds(10);
  digitalWrite(_csn, HIGH);
  delayMicroseconds(40);

  if (select()) {
    SPI.transfer(SRES);
    waitReady();   // MISO stays high for as long as the reset takes
  }
  deselect();
}

// The transmit configuration, and nothing else. Receive-side registers — AGC, bit sync,
// frequency offset compensation, the packet engine's address and length fields — are left
// at their reset values because nothing here ever receives.
void CC1101::configure() {
  // Asynchronous serial mode: the PA follows the GDO0 pin directly, so the waveform is
  // whatever the ESP puts on it. PKT_FORMAT=11, infinite packet length.
  writeRegister(REG_PKTCTRL0, 0x32);

  // GDO0 carries serial data, which in transmit makes it an input to the chip. This write
  // is not optional: the pin's reset function is a divided crystal clock, so leaving it
  // alone means the chip drives the same wire the ESP is about to drive.
  writeRegister(REG_IOCFG0, 0x0D);

  // ASK/OOK. Manchester off: the encoding is in the waveform we generate, not the chip's.
  writeRegister(REG_MDMCFG2, 0x32);
  // Channel filter and data rate. Both are receive-side in this mode, and are written to
  // keep the register set identical to the configuration the deployed bridge uses.
  writeRegister(REG_MDMCFG4, 0x87);
  writeRegister(REG_MDMCFG3, 0x93);
  writeRegister(REG_MDMCFG1, 0x02);
  writeRegister(REG_MDMCFG0, 0xF8);

  writeRegister(REG_FSCTRL1, 0x06);   // IF frequency
  writeRegister(REG_FREND1, 0x56);
  writeRegister(REG_FREND0, 0x11);    // OOK: the "on" level comes from PATABLE[1]

  // Calibrate whenever the chip goes from IDLE to TX, so transmit() never sends on a
  // stale synthesiser.
  writeRegister(REG_MCSM0, 0x18);

  // TI's recommended calibration and test values for this band. They are overwritten by
  // the automatic calibration above, but they are what it starts from.
  writeRegister(REG_FSCAL3, 0xE9);
  writeRegister(REG_FSCAL2, 0x2A);
  writeRegister(REG_FSCAL1, 0x00);
  writeRegister(REG_FSCAL0, 0x1F);
  writeRegister(REG_FSTEST, 0x59);
  writeRegister(REG_TEST2, 0x81);
  writeRegister(REG_TEST1, 0x35);
  writeRegister(REG_TEST0, 0x09);

  // Output power. In OOK the table is read twice: index 0 is the off level, which must be
  // zero or the carrier never stops, and index 1 is the on level — 0xC0 is maximum for
  // the 433 MHz band.
  const uint8_t paTable[8] = {0x00, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  writeBurst(REG_PATABLE, paTable, sizeof(paTable));
}

void CC1101::setFrequency(float megahertz) {
  const uint32_t word = (uint32_t)(megahertz * 65536.0f / CRYSTAL_MHZ);
  const uint8_t freq[3] = {(uint8_t)(word >> 16), (uint8_t)(word >> 8), (uint8_t)word};
  writeBurst(REG_FREQ2, freq, sizeof(freq));

  // A crystal offset the vendor driver applies across the 387–464 MHz band, and the value
  // the deployed bridge has been transmitting with since 2023. Somfy receivers are narrow
  // enough that it is not worth rediscovering empirically with no spectrum analyser.
  writeRegister(REG_FSCTRL0, 0x23);
}

bool CC1101::waitReady() {
  const uint32_t start = micros();
  while (digitalRead(MISO)) {
    if ((uint32_t)(micros() - start) > READY_TIMEOUT_US) {
      return false;
    }
  }
  return true;
}

bool CC1101::select() {
  SPI.beginTransaction(SPI_SETTINGS);
  digitalWrite(_csn, LOW);
  return waitReady();
}

void CC1101::deselect() {
  digitalWrite(_csn, HIGH);
  SPI.endTransaction();
}

void CC1101::strobe(uint8_t command) {
  if (select()) {
    SPI.transfer(command);
  }
  deselect();
}

void CC1101::writeRegister(uint8_t address, uint8_t value) {
  if (select()) {
    SPI.transfer(address);
    SPI.transfer(value);
  }
  deselect();
}

void CC1101::writeBurst(uint8_t address, const uint8_t *values, uint8_t count) {
  if (select()) {
    SPI.transfer((uint8_t)(address | WRITE_BURST));
    for (uint8_t i = 0; i < count; i++) {
      SPI.transfer(values[i]);
    }
  }
  deselect();
}

// Status registers share their addresses with the command strobes, so they are only
// reachable with the burst bit set — a single-access read fires the strobe instead.
uint8_t CC1101::readStatus(uint8_t address) {
  uint8_t value = 0;
  if (select()) {
    SPI.transfer((uint8_t)(address | READ_BURST));
    value = SPI.transfer(0);
  }
  deselect();
  return value;
}
