#include <string.h>
#include <unity.h>

#include "log_buffer.h"

void setUp(void) {}
void tearDown(void) {}

static const char *rendered(const LogBuffer &log, uint8_t index) {
  static char out[LOG_LINE_LEN + 24];
  log.render(index, out, sizeof(out));
  return out;
}

static void starts_empty(void) {
  LogBuffer log;
  TEST_ASSERT_EQUAL_UINT8(0, log.count());
}

// Oldest first, so a reader sees the boot banner at the top.
static void keeps_insertion_order(void) {
  LogBuffer log;
  log.push(0, "first");
  log.push(1, "second");

  TEST_ASSERT_EQUAL_UINT8(2, log.count());
  TEST_ASSERT_NOT_NULL(strstr(rendered(log, 0), "first"));
  TEST_ASSERT_NOT_NULL(strstr(rendered(log, 1), "second"));
}

// Undated lines are nearly useless: "wifi lost" means something different once versus
// fourteen times overnight.
static void stamps_each_line_with_uptime(void) {
  LogBuffer log;
  log.push(0, "boot");
  log.push(3725, "later");   // 1h 02m 05s

  TEST_ASSERT_EQUAL_STRING("[00:00:00] boot", rendered(log, 0));
  TEST_ASSERT_EQUAL_STRING("[01:02:05] later", rendered(log, 1));
}

// A broker outage retries every few seconds. Without collapsing, identical lines fill the
// whole ring and evict everything worth reading — exactly when something is wrong.
static void collapses_consecutive_repeats(void) {
  LogBuffer log;
  log.push(0, "mqtt: connect failed");
  for (uint16_t i = 1; i < 50; i++) {
    log.push(i * 5, "mqtt: connect failed");
  }

  TEST_ASSERT_EQUAL_UINT8(1, log.count());
  TEST_ASSERT_NOT_NULL(strstr(rendered(log, 0), "(x50)"));
}

// Only consecutive ones: A B A is three events, not a repeat of A.
static void only_collapses_when_adjacent(void) {
  LogBuffer log;
  log.push(0, "alpha");
  log.push(1, "beta");
  log.push(2, "alpha");
  TEST_ASSERT_EQUAL_UINT8(3, log.count());
}

// The repeat stamp should be the latest occurrence, so the reader sees when it last
// happened rather than when it started.
static void repeat_shows_the_most_recent_time(void) {
  LogBuffer log;
  log.push(0, "retrying");
  log.push(61, "retrying");
  TEST_ASSERT_EQUAL_STRING("[00:01:01] retrying (x2)", rendered(log, 0));
}

static void drops_the_oldest_when_full(void) {
  LogBuffer log;
  char line[16];
  for (uint16_t i = 0; i < LOG_LINES + 5; i++) {
    snprintf(line, sizeof(line), "line%u", i);
    log.push(i, line);
  }

  TEST_ASSERT_EQUAL_UINT8(LOG_LINES, log.count());
  TEST_ASSERT_NOT_NULL(strstr(rendered(log, 0), "line5"));
}

// Asserting only that the output fits is something snprintf guarantees regardless — an
// unterminated store would walk into the neighbouring fields and still pass. Pin the
// exact truncated text.
static void truncates_long_messages(void) {
  LogBuffer log;
  char huge[LOG_LINE_LEN * 2];
  memset(huge, 'x', sizeof(huge) - 1);
  huge[sizeof(huge) - 1] = '\0';
  log.push(0, huge);

  char expected[LOG_LINE_LEN + 24];
  memset(expected, 'x', sizeof(expected));
  memcpy(expected, "[00:00:00] ", 11);
  expected[11 + LOG_LINE_LEN - 1] = '\0';

  char out[LOG_LINE_LEN + 24];
  log.render(0, out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING(expected, out);
  TEST_ASSERT_EQUAL_UINT(11 + LOG_LINE_LEN - 1, strlen(out));
}

// B8: every message differed in its first few characters, so a narrowed comparison would
// have passed while collapsing unrelated lines that share a subsystem prefix.
static void collapse_compares_the_whole_message(void) {
  LogBuffer log;
  log.push(0, "radio     : queue full, oldest dropped");
  log.push(1, "radio     : queue full, newest dropped");
  TEST_ASSERT_EQUAL_UINT8(2, log.count());
}

static void the_repeat_counter_saturates(void) {
  LogBuffer log;
  for (uint32_t i = 0; i < 70000; i++) {
    log.push(i, "same");
  }
  char out[LOG_LINE_LEN + 24];
  log.render(0, out, sizeof(out));
  TEST_ASSERT_NOT_NULL(strstr(out, "(x65535)"));
}

static void out_of_range_is_empty(void) {
  LogBuffer log;
  log.push(0, "only");
  TEST_ASSERT_EQUAL_STRING("", rendered(log, 1));
  TEST_ASSERT_EQUAL_STRING("", rendered(log, 200));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(starts_empty);
  RUN_TEST(keeps_insertion_order);
  RUN_TEST(stamps_each_line_with_uptime);
  RUN_TEST(collapses_consecutive_repeats);
  RUN_TEST(only_collapses_when_adjacent);
  RUN_TEST(repeat_shows_the_most_recent_time);
  RUN_TEST(drops_the_oldest_when_full);
  RUN_TEST(truncates_long_messages);
  RUN_TEST(collapse_compares_the_whole_message);
  RUN_TEST(the_repeat_counter_saturates);
  RUN_TEST(out_of_range_is_empty);
  return UNITY_END();
}
