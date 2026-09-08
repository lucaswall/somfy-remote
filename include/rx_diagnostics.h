#pragma once
#include <stdint.h>

struct RadioRegister {
  const char *name;
  uint8_t address;
  uint8_t expected;
};

// Active receive values, not the high-impedance IOCFG0 used after release().
static constexpr RadioRegister RADIO_REGISTERS[] = {
    {"IOCFG0", 0x02, 0x0D}, {"PKTCTRL0", 0x08, 0x32},
    {"MDMCFG4", 0x10, 0x87}, {"MDMCFG3", 0x11, 0x93},
    {"MDMCFG2", 0x12, 0x30}, {"MDMCFG1", 0x13, 0x02},
    {"MDMCFG0", 0x14, 0xF8}, {"AGCCTRL2", 0x1B, 0xC7},
    {"AGCCTRL1", 0x1C, 0x00}, {"AGCCTRL0", 0x1D, 0xB2},
    {"FSCTRL1", 0x0B, 0x06}, {"FSCTRL0", 0x0C, 0x23},
    {"FREQ2", 0x0D, 0x10}, {"FREQ1", 0x0E, 0xAB},
    {"FREQ0", 0x0F, 0x85}};
static constexpr uint8_t RADIO_REGISTER_COUNT =
    sizeof(RADIO_REGISTERS) / sizeof(RADIO_REGISTERS[0]);

struct RadioSnapshot {
  bool attempted = false;
  uint32_t atMs = 0;
  uint16_t mute = 0;
  uint8_t marcState = 0;
  uint8_t actual[RADIO_REGISTER_COUNT] = {};
  uint8_t again[RADIO_REGISTER_COUNT] = {};
  uint16_t mismatches = 0;
  uint16_t invalid = 0;

  void add(uint8_t i, bool ok, bool okAgain, uint8_t value, uint8_t repeat) {
    if (i >= RADIO_REGISTER_COUNT) {
      return;
    }
    const uint16_t bit = (uint16_t)(1u << i);
    invalid &= ~bit;
    mismatches &= ~bit;
    actual[i] = value;
    again[i] = repeat;
    if (!ok || !okAgain || value != repeat) {
      invalid |= bit;
    } else if (value != RADIO_REGISTERS[i].expected) {
      mismatches |= bit;
    }
  }
};

// Receive-enabled dwell is NOT proof of decoded RF or continuous MARCSTATE RX.
class RxDwell {
 public:
  enum Mode : uint8_t { Receiving, Muted, Suspended, Inactive, Count };
  struct Window {
    uint32_t ms[Count] = {};
    uint32_t endMs = 0;
    uint32_t duration() const { return ms[0] + ms[1] + ms[2] + ms[3]; }
    uint16_t permille(Mode mode) const {
      return duration() == 0 ? 0 : (uint16_t)((uint64_t)ms[mode] * 1000 / duration());
    }
  };
  void begin(uint32_t now) {
    *this = RxDwell{};
    _at = now;
  }
  void set(uint32_t now, Mode mode) {
    advance(now);
    _mode = mode;
  }
  void advance(uint32_t now) {
    uint32_t remaining = now - _at;
    while (remaining) {
      const uint32_t room = 60000 - _current.duration();
      const uint32_t part = remaining < room ? remaining : room;
      _current.ms[_mode] += part;
      _at += part;
      _current.endMs = _at;
      remaining -= part;
      if (_current.duration() == 60000) {
        _last = _current;
        _current = Window{};
      }
    }
  }
  const Window &last() const { return _last; }
  const Window &current() const { return _current; }
 private:
  uint32_t _at = 0;
  Mode _mode = Inactive;
  Window _current;
  Window _last;
};
