#include <unity.h>

#include "rolling_code.h"

void setUp(void) {}
void tearDown(void) {}

// The layout the 2023 sketch wrote, pinned. A shutter only obeys codes ahead of the one
// it last saw, so reading a remote's counter from the wrong address means walking to the
// motor and pairing it again — this test is the guard rail on that.
static void addresses_match_the_deployed_layout(void) {
  TEST_ASSERT_EQUAL_UINT16(0, rollingCodeAddress(0));
  TEST_ASSERT_EQUAL_UINT16(2, rollingCodeAddress(1));
  TEST_ASSERT_EQUAL_UINT16(22, rollingCodeAddress(11));
  TEST_ASSERT_EQUAL_UINT16(58, rollingCodeAddress(ROLLING_CODE_MAX_REMOTES - 1));
}

// The old count field sat at offset 60. Nothing reads it any more, but a counter written
// over it would still corrupt whatever a future firmware puts there.
static void the_last_remote_stops_short_of_the_reserved_slot(void) {
  const uint16_t last = rollingCodeAddress(ROLLING_CODE_MAX_REMOTES - 1);
  TEST_ASSERT_TRUE(last + ROLLING_CODE_BYTES <= 60);
  TEST_ASSERT_TRUE(60 + 2 <= ROLLING_CODE_EEPROM_SIZE);
}

static void codes_advance_by_one(void) {
  TEST_ASSERT_EQUAL_UINT16(1, rollingCodeNext(0));
  TEST_ASSERT_EQUAL_UINT16(0x4E21, rollingCodeNext(0x4E20));
}

// The counter is sixteen bits on the wire, so it has to wrap rather than saturate or
// widen. A shutter resynchronises across the wrap; a counter stuck at 0xFFFF never moves
// again.
static void codes_wrap_at_sixteen_bits(void) {
  TEST_ASSERT_EQUAL_UINT16(0, rollingCodeNext(0xFFFF));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(addresses_match_the_deployed_layout);
  RUN_TEST(the_last_remote_stops_short_of_the_reserved_slot);
  RUN_TEST(codes_advance_by_one);
  RUN_TEST(codes_wrap_at_sixteen_bits);
  return UNITY_END();
}
