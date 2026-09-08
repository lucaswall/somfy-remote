#include <unity.h>
#include "rx_diagnostics.h"

void setUp() {}
void tearDown() {}

static void minute_separates_rx_mute_and_suspend() {
  RxDwell d;
  d.begin(0);
  d.set(0, RxDwell::Receiving);
  d.set(2000, RxDwell::Muted);
  d.set(59000, RxDwell::Suspended);
  d.advance(60000);
  const auto &w = d.last();
  TEST_ASSERT_EQUAL_UINT32(60000, w.duration());
  TEST_ASSERT_EQUAL_UINT32(2000, w.ms[RxDwell::Receiving]);
  TEST_ASSERT_EQUAL_UINT32(57000, w.ms[RxDwell::Muted]);
  TEST_ASSERT_EQUAL_UINT32(1000, w.ms[RxDwell::Suspended]);
  TEST_ASSERT_EQUAL_UINT32(60000, w.endMs);
}
static void partial_and_multiple_windows() {
  RxDwell d;
  d.begin(100);
  d.set(100, RxDwell::Receiving);
  d.advance(160100);
  TEST_ASSERT_EQUAL_UINT32(60000, d.last().duration());
  TEST_ASSERT_EQUAL_UINT32(120100, d.last().endMs);
  TEST_ASSERT_EQUAL_UINT32(40000, d.current().duration());
  TEST_ASSERT_EQUAL_UINT32(40000, d.current().ms[RxDwell::Receiving]);
}
static void millis_rollover() {
  RxDwell d;
  const uint32_t start = UINT32_MAX - 1000;
  d.begin(start);
  d.set(start, RxDwell::Muted);
  d.set(start + 2000, RxDwell::Receiving);
  d.advance(start + 60000);
  TEST_ASSERT_EQUAL_UINT32(2000, d.last().ms[RxDwell::Muted]);
  TEST_ASSERT_EQUAL_UINT32(58000, d.last().ms[RxDwell::Receiving]);
}
static void same_time_transition_has_no_dwell() {
  RxDwell d;
  d.begin(0);
  d.set(0, RxDwell::Receiving);
  d.set(0, RxDwell::Muted);
  d.advance(100);
  TEST_ASSERT_EQUAL_UINT32(0, d.current().ms[RxDwell::Receiving]);
  TEST_ASSERT_EQUAL_UINT32(100, d.current().ms[RxDwell::Muted]);
  TEST_ASSERT_EQUAL_UINT32(0, d.last().duration());
}
static void snapshot_distinguishes_failure_from_mismatch() {
  RadioSnapshot s;
  s.attempted = true;
  for (uint8_t i = 0; i < RADIO_REGISTER_COUNT; ++i) {
    s.add(i, true, true, RADIO_REGISTERS[i].expected, RADIO_REGISTERS[i].expected);
  }
  TEST_ASSERT_EQUAL_UINT16(0, s.mismatches);
  TEST_ASSERT_EQUAL_UINT16(0, s.invalid);
  s.add(0, true, true, 0x2E, 0x2E);
  TEST_ASSERT_EQUAL_UINT16(1, s.mismatches);
  s.add(1, false, true, 0, 0);
  s.add(2, true, true, 0x87, 0x86);
  TEST_ASSERT_EQUAL_UINT16(6, s.invalid);
  TEST_ASSERT_EQUAL_UINT16(1, s.mismatches);
}
static void expected_registers_pin_receive_configuration() {
  TEST_ASSERT_EQUAL_UINT8(15, RADIO_REGISTER_COUNT);
  TEST_ASSERT_EQUAL_UINT8(0x02, RADIO_REGISTERS[0].address);
  TEST_ASSERT_EQUAL_UINT8(0x0D, RADIO_REGISTERS[0].expected);
  TEST_ASSERT_EQUAL_UINT8(0x08, RADIO_REGISTERS[1].address);
  TEST_ASSERT_EQUAL_UINT8(0x32, RADIO_REGISTERS[1].expected);
  TEST_ASSERT_EQUAL_UINT8(0x1B, RADIO_REGISTERS[7].address);
  TEST_ASSERT_EQUAL_UINT8(0xC7, RADIO_REGISTERS[7].expected);
  const uint32_t word = (uint32_t)(433.42f * 65536.0f / 26.0f);
  for (uint8_t i = 0; i < 3; ++i) {
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(word >> (16 - 8 * i)),
                            RADIO_REGISTERS[12 + i].expected);
  }
  for (const auto &reg : RADIO_REGISTERS) {
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(0x2E, reg.address);
  }
}
static void snapshot_replaces_result_and_ignores_out_of_range() {
  RadioSnapshot s;
  s.add(0, true, true, 0, 0);
  s.add(0, false, true, 0, 0);
  TEST_ASSERT_EQUAL_UINT16(0, s.mismatches);
  TEST_ASSERT_EQUAL_UINT16(1, s.invalid);
  s.add(0, true, true, 0x0D, 0x0D);
  s.add(RADIO_REGISTER_COUNT, true, true, 0, 0);
  TEST_ASSERT_EQUAL_UINT16(0, s.mismatches);
  TEST_ASSERT_EQUAL_UINT16(0, s.invalid);
}
static void duty_is_bounded_and_empty_window_is_zero() {
  RxDwell::Window w;
  TEST_ASSERT_EQUAL_UINT16(0, w.permille(RxDwell::Receiving));
  w.ms[RxDwell::Receiving] = 2000;
  w.ms[RxDwell::Muted] = 58000;
  TEST_ASSERT_EQUAL_UINT16(33, w.permille(RxDwell::Receiving));
  TEST_ASSERT_EQUAL_UINT16(966, w.permille(RxDwell::Muted));
}
static void begin_resets_previous_windows() {
  RxDwell d;
  d.begin(0);
  d.set(0, RxDwell::Receiving);
  d.advance(60000);
  d.begin(70000);
  d.advance(71000);
  TEST_ASSERT_EQUAL_UINT32(0, d.last().duration());
  TEST_ASSERT_EQUAL_UINT32(1000, d.current().ms[RxDwell::Inactive]);
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(minute_separates_rx_mute_and_suspend);
  RUN_TEST(partial_and_multiple_windows);
  RUN_TEST(millis_rollover);
  RUN_TEST(same_time_transition_has_no_dwell);
  RUN_TEST(snapshot_distinguishes_failure_from_mismatch);
  RUN_TEST(expected_registers_pin_receive_configuration);
  RUN_TEST(snapshot_replaces_result_and_ignores_out_of_range);
  RUN_TEST(duty_is_bounded_and_empty_window_is_zero);
  RUN_TEST(begin_resets_previous_windows);
  return UNITY_END();
}
