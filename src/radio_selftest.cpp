// Standalone CC1101 self-test, built by `make radio`.
//
// Every failure it reports is wiring or power, never software. It exists because the
// firmware cannot answer this question: a bridge with a dead radio connects to WiFi,
// serves its page, accepts commands and reports them as sent, and the only symptom is
// shutters that do not move.

#include <Arduino.h>

#include "CC1101.h"

static const uint8_t PIN_CSN = 15;
static const uint8_t PIN_DATA = 5;
static const float SOMFY_MHZ = 433.42f;

// The part identifiers a genuine CC1101 returns. PARTNUM is 0x00 on every one of them,
// which is why it is useless on its own — VERSION is the one that carries information.
static const uint8_t EXPECTED_PARTNUM = 0x00;

static CC1101 radio(PIN_CSN);

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println(F("=== somfy-remote: radio self-test ==="));
  Serial.printf("build   : %s %s\n", __DATE__, __TIME__);
  Serial.printf("wiring  : CSN=GPIO%u GDO0=GPIO%u SCK=GPIO14 MOSI=GPIO13 MISO=GPIO12\n",
                PIN_CSN, PIN_DATA);

  const bool started = radio.begin(SOMFY_MHZ);
  const uint8_t partnum = radio.readStatus(CC1101_PARTNUM);
  const uint8_t version = radio.readStatus(CC1101_VERSION);

  Serial.printf("begin() : %s\n", started ? "true" : "false");
  Serial.printf("PARTNUM : 0x%02X\n", partnum);
  Serial.printf("VERSION : 0x%02X\n", version);

  if (version == 0x00) {
    Serial.println(F("RESULT: FAIL — MISO reads low. Module unpowered, or MISO/GND swapped."));
    return;
  }
  if (version == 0xFF) {
    Serial.println(F("RESULT: FAIL — MISO floats. Nothing is answering on the bus."));
    return;
  }
  if (partnum != EXPECTED_PARTNUM) {
    Serial.printf("RESULT: FAIL — PARTNUM 0x%02X, expected 0x%02X. Not a CC1101.\n",
                  partnum, EXPECTED_PARTNUM);
    return;
  }

  // The register set is only proven once the chip acts on it: entering transmit runs a
  // full synthesiser calibration, and a chip that cannot calibrate never reaches TX.
  const bool tx = radio.transmit();
  Serial.printf("MARCSTATE after STX: 0x%02X\n", radio.readStatus(CC1101_MARCSTATE));
  radio.idle();

  if (!tx) {
    Serial.println(F("RESULT: FAIL — the chip answers but will not enter transmit."));
    return;
  }

  Serial.println(F("RESULT: PASS — CC1101 is wired, configured and reaches transmit."));
  // GDO0 is a plain data line the chip never reads back, so no register test can tell
  // whether it is connected. It is the CE of this build: check it by eye.
  Serial.printf("Not covered: GDO0 on GPIO%u. Nothing can test it from here.\n", PIN_DATA);
}

void loop() {
  delay(1000);
}
