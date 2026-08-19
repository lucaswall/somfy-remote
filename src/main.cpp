// somfy-remote — a Somfy RTS bridge.
//
// Construction and the loop, nothing else. Behaviour lives in Remotes (the emulated
// remotes and their rolling codes), SomfyRadio (the CC1101 and the waveform), Net (WiFi
// and OTA), HaMqtt (Home Assistant) and WebUi (the debug interface).
//
// Wemos D1 mini: LED_BUILTIN is GPIO2, active low.

#include <Arduino.h>

#include "build_info.h"
#include "log.h"

#include "ha_mqtt.h"
#include "net.h"
#include "radio.h"
#include "receiver.h"
#include "remotes.h"
#include "secrets.h"
#include "store.h"
#include "timing.h"
#include "web.h"

// GPIO numbers, not D-numbers: every CC1101 wiring guide gives these as D8 and D1, which
// mean different pins on a D1 R1.
//
// GPIO15 is a boot-strap pin that must be low at reset. It works as chip select because
// the CC1101 does not pull it up and nothing drives it until the sketch runs — but a board
// that will not boot at all is the first thing to check here.
static const uint8_t PIN_CSN = 15;
// The CC1101's GDO0, carrying the bit-banged waveform.
static const uint8_t PIN_DATA = 5;

// The network name. Deliberately not MQTT_DEVICE_ID: that is the identity Home Assistant
// keys twelve devices on and it contains underscores, which do not belong in a hostname.
static const char HOST[] = "somfy-remote";

static Net net;
static SomfyRadio radio(PIN_CSN, PIN_DATA);
static Store store;
static Remotes remotes(radio, store);
static Receiver receiver(radio, remotes);
static HaMqtt mqtt(remotes, store, receiver, HOST);
static WebUi web(remotes, store, mqtt, net, receiver, HOST);

// A radio that fails to start is not recoverable by hand: the board is in a case. Retry it
// from the loop instead of logging once and running blind forever.
static const uint32_t RADIO_RETRY_MS = 30000;
static uint32_t lastRadioTry = 0;

// Reporting heap only on new lows keeps a healthy board quiet while a leak shows up as a
// steady descent.
static const uint32_t HEAP_REPORT_STEP = 1024;
static uint32_t heapLowWater = 0;

// A heartbeat, so an idle log reads as idle rather than as a stalled page. Uptime is
// deliberately absent: every line already carries it as a timestamp, and repeating it here
// would stop identical readings collapsing.
static const uint32_t HEALTH_MS = 5UL * 60 * 1000;
static uint32_t lastHealth = 0;

// The only way to price the receive interrupt: an ISR firing a few hundred times a second is
// invisible in every other number here.
static uint32_t loops = 0;

static void banner() {
  Serial.println();
  logLine("=== somfy-remote ===");
  logLine("build     : %s", BUILD_STAMP);
  logLine("host      : %s", HOST);
  logLine("heap      : %u bytes", ESP.getFreeHeap());
  logLine("reset     : %s", ESP.getResetReason().c_str());
  // The pin map differs between this board and the D1 R1, and building for the wrong one
  // produces a firmware that runs, connects, and transmits on the UART.
  logLine("pinmap    : D1=%u D8=%u LED_BUILTIN=%u", D1, D8, LED_BUILTIN);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);   // active low, so start off

  Serial.begin(115200);
  delay(200);
  banner();

  // Before radio.begin(), which must be able to stop the receiver while SRES leaves a
  // divided crystal clock on the data pin.
  receiver.begin(PIN_DATA);
  radio.listener(&receiver);

  if (radio.begin()) {
    logLine("radio     : CC1101 ready on 433.42 MHz");
    // Receive on a broken data wire fails silently, unlike transmit.
    if (radio.testDataPin()) {
      logLine("radio     : GDO0 wire verified both ways");
    } else {
      logError("radio     : GDO0 does not read back — check the wire to D1/GPIO5");
    }
  } else {
    logError("radio     : CC1101 did not answer, will retry — run `make radio`");
  }

  if (!store.begin()) {
    logError("store     : no usable record store — the device cannot transmit");
  }
  remotes.begin();

  heapLowWater = ESP.getFreeHeap();
  net.begin(HOST, WIFI_SSID, WIFI_PASSWORD, OTA_PASSWORD);
}

void loop() {
  const uint32_t now = millis();
  loops++;

  net.loop();   // before web: this is what starts mDNS, which web then advertises on
  web.loop();
  mqtt.loop();

  if (!radio.ready() && elapsed(now, lastRadioTry, RADIO_RETRY_MS)) {
    lastRadioTry = now;
    if (radio.begin()) {
      logLine("radio     : recovered");
    }
  }

  remotes.loop();
  receiver.loop();   // decodes what the interrupt handler recorded
  store.loop();      // compaction, never on the press path

  if (elapsed(now, lastHealth, HEALTH_MS)) {
    lastHealth = now;
    // Terse because it must fit LOG_LINE_LEN: logLine() prints the buffer it formats into,
    // so anything longer is truncated on the serial console as well as in the ring.
    logLine("health    : heap %lu/%lu rssi %d w%c m%c r%c q%u f%u store %c/%u %lu/s%s",
            (unsigned long)ESP.getFreeHeap(), (unsigned long)heapLowWater, net.rssi(),
            net.connected() ? '+' : '-', mqtt.connected() ? '+' : '-',
            radio.ready() ? '+' : '-', remotes.pending(), errorBuffer().count(),
            store.activeName(), store.freeSlots(),
            (unsigned long)(loops / (HEALTH_MS / 1000)),
            store.degraded() ? " DEGRADED" : "");
    loops = 0;
    const Receiver::Stats rx = receiver.stats();
    logLine("receiver  : %lu edges/s, %lu frames, %lu presses, %u mutes%s",
            (unsigned long)receiver.edgesPerSecond(), (unsigned long)rx.frames,
            (unsigned long)rx.presses, rx.mutes, rx.muted ? " MUTED" : "");
  }

  const uint32_t heap = ESP.getFreeHeap();
  if (heap + HEAP_REPORT_STEP < heapLowWater) {
    heapLowWater = heap;
    logLine("heap      : new low %lu bytes", (unsigned long)heap);
  }
}
