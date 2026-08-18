#include <unity.h>

#include "command_queue.h"

void setUp(void) {}
void tearDown(void) {}

static void empty_queue_yields_nothing(void) {
  CommandQueue queue;
  QueuedCommand out;
  TEST_ASSERT_TRUE(queue.empty());
  TEST_ASSERT_FALSE(queue.pop(&out));
}

// The reason the queue exists: closing every shutter in the house is a dozen commands
// arriving in one burst, and a single slot would send only the last one.
static void holds_a_burst_in_order(void) {
  CommandQueue queue;
  for (uint8_t i = 0; i < 12; i++) {
    TEST_ASSERT_TRUE(queue.push(i, SOMFY_DOWN));
  }
  TEST_ASSERT_EQUAL_UINT8(12, queue.count());

  QueuedCommand out;
  for (uint8_t i = 0; i < 12; i++) {
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT8(i, out.remote);
    TEST_ASSERT_EQUAL(SOMFY_DOWN, out.command);
  }
  TEST_ASSERT_TRUE(queue.empty());
}

static void overflow_drops_the_oldest(void) {
  CommandQueue queue;
  for (uint8_t i = 0; i < COMMAND_QUEUE_LEN + 2; i++) {
    const bool ok = queue.push(i, SOMFY_UP);
    TEST_ASSERT_EQUAL(i < COMMAND_QUEUE_LEN, ok);
  }

  QueuedCommand out;
  TEST_ASSERT_TRUE(queue.pop(&out));
  TEST_ASSERT_EQUAL_UINT8(2, out.remote);   // 0 and 1 were pushed out

  uint8_t drained = 1;
  while (queue.pop(&out)) {
    drained++;
  }
  TEST_ASSERT_EQUAL_UINT8(COMMAND_QUEUE_LEN, drained);
}

// Indices wrap, so the queue has to keep working long after the buffer has been lapped.
static void survives_wrapping(void) {
  CommandQueue queue;
  QueuedCommand out;
  for (uint8_t round = 0; round < 100; round++) {
    TEST_ASSERT_TRUE(queue.push((uint8_t)(round % 12), SOMFY_MY));
    TEST_ASSERT_TRUE(queue.pop(&out));
    TEST_ASSERT_EQUAL_UINT8(round % 12, out.remote);
  }
  TEST_ASSERT_TRUE(queue.empty());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(empty_queue_yields_nothing);
  RUN_TEST(holds_a_burst_in_order);
  RUN_TEST(overflow_drops_the_oldest);
  RUN_TEST(survives_wrapping);
  return UNITY_END();
}
