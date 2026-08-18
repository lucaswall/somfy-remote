#include <unity.h>

#include "timing.h"

void setUp(void) {}
void tearDown(void) {}

static void not_elapsed_before_interval(void) {
  TEST_ASSERT_FALSE(elapsed(499, 0, 500));
}

static void elapsed_exactly_on_interval(void) {
  TEST_ASSERT_TRUE(elapsed(500, 0, 500));
}

// The case that matters: now has wrapped past UINT32_MAX and is numerically far below
// last. A naive (now - last >= interval) on signed types, or (now >= last + interval),
// gets this wrong and freezes the board for ~49.7 days.
static void survives_rollover(void) {
  const uint32_t last = UINT32_MAX - 100;
  TEST_ASSERT_FALSE(elapsed(last + 99, last, 500));
  TEST_ASSERT_TRUE(elapsed(last + 500, last, 500));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(not_elapsed_before_interval);
  RUN_TEST(elapsed_exactly_on_interval);
  RUN_TEST(survives_rollover);
  return UNITY_END();
}
