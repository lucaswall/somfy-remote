#include <unity.h>

#include "somfy_pulses.h"

void setUp(void) {}
void tearDown(void) {}

static SomfyPulse pulses[SOMFY_MAX_PULSES];

static uint32_t totalMicroseconds(size_t count) {
  uint32_t total = 0;
  for (size_t i = 0; i < count; i++) {
    total += pulses[i].microseconds;
  }
  return total;
}

// The repeat frame is the worst case, and SOMFY_MAX_PULSES is what the transmitter sizes
// its buffer from. If this drifts, the buffer silently stops fitting.
static void repeat_frame_is_the_worst_case(void) {
  const uint8_t frame[SOMFY_FRAME_LEN] = {0};
  const size_t count =
      somfyBuildPulses(frame, SOMFY_REPEAT_SYNC, pulses, SOMFY_MAX_PULSES);
  TEST_ASSERT_EQUAL_size_t(SOMFY_MAX_PULSES, count);
}

static void refuses_to_overrun_the_buffer(void) {
  const uint8_t frame[SOMFY_FRAME_LEN] = {0};
  TEST_ASSERT_EQUAL_size_t(
      0, somfyBuildPulses(frame, SOMFY_REPEAT_SYNC, pulses, SOMFY_MAX_PULSES - 1));
}

static void first_frame_opens_with_hardware_sync(void) {
  const uint8_t frame[SOMFY_FRAME_LEN] = {0};
  const size_t count = somfyBuildPulses(frame, SOMFY_FIRST_SYNC, pulses, SOMFY_MAX_PULSES);
  TEST_ASSERT_EQUAL_size_t(2 * SOMFY_FIRST_SYNC + 2 + 2 * SOMFY_FRAME_BITS + 1, count);

  for (uint8_t i = 0; i < SOMFY_FIRST_SYNC; i++) {
    TEST_ASSERT_TRUE(pulses[i * 2].high);
    TEST_ASSERT_EQUAL_UINT16(SOMFY_HW_SYNC_US, pulses[i * 2].microseconds);
    TEST_ASSERT_FALSE(pulses[i * 2 + 1].high);
    TEST_ASSERT_EQUAL_UINT16(SOMFY_HW_SYNC_US, pulses[i * 2 + 1].microseconds);
  }

  const size_t sw = SOMFY_FIRST_SYNC * 2;
  TEST_ASSERT_TRUE(pulses[sw].high);
  TEST_ASSERT_EQUAL_UINT16(SOMFY_SW_SYNC_HIGH_US, pulses[sw].microseconds);
  TEST_ASSERT_FALSE(pulses[sw + 1].high);
  TEST_ASSERT_EQUAL_UINT16(SOMFY_SYMBOL_US, pulses[sw + 1].microseconds);

  TEST_ASSERT_FALSE(pulses[count - 1].high);
  TEST_ASSERT_EQUAL_UINT16(SOMFY_SILENCE_US, pulses[count - 1].microseconds);
}

// Somfy's Manchester is the inverted convention: a 1 is low-then-high. Sending the two
// halves the right way round is the difference between Up and nothing at all, and no
// receiver will ever tell us which we sent.
static void encodes_a_one_low_then_high(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  for (uint8_t i = 0; i < SOMFY_FRAME_LEN; i++) {
    frame[i] = 0xFF;
  }
  const size_t count = somfyBuildPulses(frame, SOMFY_FIRST_SYNC, pulses, SOMFY_MAX_PULSES);
  const size_t data = SOMFY_FIRST_SYNC * 2 + 2;

  for (size_t i = data; i < count - 1; i += 2) {
    TEST_ASSERT_FALSE(pulses[i].high);
    TEST_ASSERT_TRUE(pulses[i + 1].high);
    TEST_ASSERT_EQUAL_UINT16(SOMFY_SYMBOL_US, pulses[i].microseconds);
    TEST_ASSERT_EQUAL_UINT16(SOMFY_SYMBOL_US, pulses[i + 1].microseconds);
  }
}

static void encodes_a_zero_high_then_low(void) {
  const uint8_t frame[SOMFY_FRAME_LEN] = {0};
  const size_t count = somfyBuildPulses(frame, SOMFY_FIRST_SYNC, pulses, SOMFY_MAX_PULSES);
  const size_t data = SOMFY_FIRST_SYNC * 2 + 2;

  for (size_t i = data; i < count - 1; i += 2) {
    TEST_ASSERT_TRUE(pulses[i].high);
    TEST_ASSERT_FALSE(pulses[i + 1].high);
  }
}

// Bits go out most significant first, so the top bit of byte 0 is the first one on the
// wire. Sending them the other way round produces a frame with a valid shape and no
// meaning.
static void sends_the_most_significant_bit_first(void) {
  uint8_t frame[SOMFY_FRAME_LEN] = {0};
  frame[0] = 0x80;
  const size_t count = somfyBuildPulses(frame, SOMFY_FIRST_SYNC, pulses, SOMFY_MAX_PULSES);
  const size_t data = SOMFY_FIRST_SYNC * 2 + 2;

  TEST_ASSERT_FALSE(pulses[data].high);        // the 1
  TEST_ASSERT_TRUE(pulses[data + 1].high);
  TEST_ASSERT_TRUE(pulses[data + 2].high);     // and the 0 behind it
  TEST_ASSERT_FALSE(pulses[data + 3].high);
  TEST_ASSERT_EQUAL_size_t(2 * SOMFY_FIRST_SYNC + 2 + 2 * SOMFY_FRAME_BITS + 1, count);
}

// Every bit costs the same air time whatever its value, which is what makes a frame's
// duration a fixed number rather than a function of its contents.
static void frame_duration_is_independent_of_the_data(void) {
  uint8_t zeros[SOMFY_FRAME_LEN] = {0};
  uint8_t ones[SOMFY_FRAME_LEN];
  for (uint8_t i = 0; i < SOMFY_FRAME_LEN; i++) {
    ones[i] = 0xFF;
  }

  const size_t a = somfyBuildPulses(zeros, SOMFY_FIRST_SYNC, pulses, SOMFY_MAX_PULSES);
  const uint32_t first = totalMicroseconds(a);
  const size_t b = somfyBuildPulses(ones, SOMFY_FIRST_SYNC, pulses, SOMFY_MAX_PULSES);
  TEST_ASSERT_EQUAL_UINT32(first, totalMicroseconds(b));
  TEST_ASSERT_EQUAL_UINT32(87525, first);

  somfyBuildPulses(zeros, SOMFY_REPEAT_SYNC, pulses, SOMFY_MAX_PULSES);
  TEST_ASSERT_EQUAL_UINT32(113125, totalMicroseconds(SOMFY_MAX_PULSES));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(repeat_frame_is_the_worst_case);
  RUN_TEST(refuses_to_overrun_the_buffer);
  RUN_TEST(first_frame_opens_with_hardware_sync);
  RUN_TEST(encodes_a_one_low_then_high);
  RUN_TEST(encodes_a_zero_high_then_low);
  RUN_TEST(sends_the_most_significant_bit_first);
  RUN_TEST(frame_duration_is_independent_of_the_data);
  return UNITY_END();
}
