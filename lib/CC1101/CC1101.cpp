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
static const uint8_t SRX = 0x34;
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
// Datasheet §29 register map, and worth reading twice: these four are consecutive and it is
// very easy to be off by one. FOCCFG 0x19, BSCFG 0x1A, AGCCTRL2 0x1B, AGCCTRL1 0x1C,
// AGCCTRL0 0x1D, and 0x1E is WOREVT1 — nothing to do with the AGC at all.
static const uint8_t REG_FOCCFG = 0x19;
static const uint8_t REG_BSCFG = 0x1A;
static const uint8_t REG_AGCCTRL2 = 0x1B;
static const uint8_t REG_AGCCTRL1 = 0x1C;
static const uint8_t REG_AGCCTRL0 = 0x1D;
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

// Entering TX or RX from IDLE runs a full calibration, around 720 µs by the datasheet's
// Table 35. Ten milliseconds is generous enough that a timeout means the chip has stopped
// answering rather than that it is merely busy.
static const uint32_t STATE_TIMEOUT_US = 10000;

// GDO0's two configurations. 0x0D is "serial data output, asynchronous serial mode" and is
// what the chip drives while receiving; 0x2E is high impedance, which is what lets the ESP
// drive the same wire to transmit. Datasheet Table 41.
static const uint8_t GDO0_SERIAL_DATA = 0x0D;
static const uint8_t GDO0_HIGH_Z = 0x2E;
// "HW to 0", with bit 6 being GDOx_INV — so 0x2F drives the pin low and 0x6F drives it
// high, whatever the radio is doing. Table 41 again.
static const uint8_t GDO0_DRIVE_LOW = 0x2F;
static const uint8_t GDO0_DRIVE_HIGH = 0x6F;

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

// IOCFG0 goes back to serial data before STX, so the register state the chip transmits with
// is byte for byte the one the deployed bridge has been using since 2023. That matters more
// than it looks: transmit is the half that already works, every motor in the house depends
// on it, and receive must not be allowed to alter it.
//
// The 3-state value is therefore an *idle* setting, not a transmit one. §11.2 says the chip
// takes GDO0 as an input while transmitting, which is why driving it here has always been
// safe; the datasheet says nothing about IDLE, and IDLE is where this radio spends almost
// all of its life. What release() removes is that window.
bool CC1101::transmit() {
  strobe(SIDLE);
  writeRegister(REG_IOCFG0, GDO0_SERIAL_DATA);
  strobe(STX);
  // Returning before the synthesiser settles would put the first sync pulse on the air with
  // it still moving.
  return waitForState(CC1101_STATE_TX);
}

// The pin turns round here: from this point the chip drives GDO0 and the ESP must already
// have made its own side an input.
bool CC1101::receive() {
  strobe(SIDLE);
  writeRegister(REG_IOCFG0, GDO0_SERIAL_DATA);
  strobe(SRX);
  return waitForState(CC1101_STATE_RX);
}

// IDLE first, then release the pin — in that order, because a GDO output is only reliably
// quiet once the state machine has stopped feeding it.
void CC1101::release() {
  strobe(SIDLE);
  writeRegister(REG_IOCFG0, GDO0_HIGH_Z);
}

void CC1101::driveGdo0(bool high) {
  writeRegister(REG_IOCFG0, high ? GDO0_DRIVE_HIGH : GDO0_DRIVE_LOW);
}

void CC1101::idle() { strobe(SIDLE); }

bool CC1101::waitForState(uint8_t want) {
  const uint32_t start = micros();
  while ((uint32_t)(micros() - start) < STATE_TIMEOUT_US) {
    if (readStatus(CC1101_MARCSTATE) == want) {
      return true;
    }
  }
  return false;
}

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

// The whole configuration, transmit and receive. The transmit half is unchanged from what
// the deployed bridge has been sending since 2023 and must stay that way; every register
// added for receive acts on the demodulator and cannot alter the transmitted waveform.
//
// The receive values are the ones the driver this project's transmit configuration was
// originally taken from uses for asynchronous OOK, with one correction noted at FOCCFG.
void CC1101::configure() {
  // Asynchronous serial mode: the PA follows the GDO0 pin directly, so the waveform is
  // whatever the ESP puts on it. PKT_FORMAT=11, infinite packet length.
  writeRegister(REG_PKTCTRL0, 0x32);

  // GDO0 starts 3-stated, not as serial data. The pin's reset function is a divided crystal
  // clock, so leaving it alone would put the chip's driver on the wire the ESP is about to
  // drive — and 0x0D would leave the chip nominally driving it through every idle moment
  // between presses. receive() switches it to serial data for as long as the chip owns it.
  writeRegister(REG_IOCFG0, GDO0_HIGH_Z);

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

  // Calibrate whenever the chip goes from IDLE to TX or RX, so neither is entered on a
  // stale synthesiser.
  writeRegister(REG_MCSM0, 0x18);

  // --- receive ---------------------------------------------------------------------------
  //
  // None of these can affect transmit: they are the demodulator, its gain control and its
  // bit synchroniser. FREND1 above is the receive front end and already holds this value.

  // Frequency offset compensation, with FOC_LIMIT deliberately 0 rather than the 2 every
  // library copies here. The datasheet's own note on this register: "Frequency offset
  // compensation is not supported for ASK/OOK. Always use FOC_LIMIT=0 with these
  // modulation formats."
  writeRegister(REG_FOCCFG, 0x14);
  writeRegister(REG_BSCFG, 0x1C);

  // The AGC, and the single most consequential choice on the receive side.
  //
  // MAX_DVGA_GAIN = 3 disables the three highest digital gain steps. Without it the AGC
  // amplifies an empty band until noise crosses the decision boundary and GDO0 toggles
  // continuously — which on a chip that is also running a WiFi stack is how a receiver
  // starves the thing it shares a core with.
  //
  // It is not free: against 0x07 this costs roughly 18 dB of sensitivity (datasheet
  // §17.4.1, Tables 32 and 33). That trade — range given up for a quiet data line — is the
  // one number to revisit if a wall button at the far end of the house cannot be heard.
  writeRegister(REG_AGCCTRL2, 0xC7);
  writeRegister(REG_AGCCTRL1, 0x00);
  // FILTER_LENGTH = 2, which for OOK is not a filter length at all but the decision
  // boundary: 12 dB between what counts as carrier and what counts as silence.
  writeRegister(REG_AGCCTRL0, 0xB2);

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
