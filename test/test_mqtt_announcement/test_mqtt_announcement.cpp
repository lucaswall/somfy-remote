#include <unity.h>

#include "mqtt_announcement.h"

using mqtt_announcement::Kind;
using mqtt_announcement::Sequence;

void setUp() {}
void tearDown() {}

void test_sequence_emits_one_bounded_step_at_a_time() {
  Sequence sequence;
  sequence.start(2, 1);

  unsigned discovery = 0, state = 0, counter = 0, controlConfig = 0;
  unsigned controlDiscovery = 0, bridge = 0, health = 0, availability = 0;
  bool completed = false;
  while (sequence.active()) {
    const auto step = sequence.current();
    switch (step.kind) {
      case Kind::RemoteDiscovery: discovery++; break;
      case Kind::RemoteState: state++; break;
      case Kind::RemoteCounter: counter++; break;
      case Kind::ControlConfig: controlConfig++; break;
      case Kind::ControlDiscovery: controlDiscovery++; break;
      case Kind::BridgeDiscovery: bridge++; break;
      case Kind::Health: health++; break;
      case Kind::Availability: availability++; break;
      case Kind::None: TEST_FAIL_MESSAGE("inactive step in active sequence");
    }
    completed = sequence.advance();
  }

  TEST_ASSERT_EQUAL_UINT(6, discovery);
  TEST_ASSERT_EQUAL_UINT(6, state);
  TEST_ASSERT_EQUAL_UINT(2, counter);
  TEST_ASSERT_EQUAL_UINT(1, controlConfig);
  TEST_ASSERT_EQUAL_UINT(1, controlDiscovery);
  TEST_ASSERT_EQUAL_UINT(10, bridge);
  TEST_ASSERT_EQUAL_UINT(1, health);
  TEST_ASSERT_EQUAL_UINT(1, availability);
  TEST_ASSERT_TRUE(completed);
}

void test_sequence_skips_empty_remote_and_control_groups() {
  Sequence sequence;
  sequence.start(0, 0);

  TEST_ASSERT_EQUAL(Kind::BridgeDiscovery, sequence.current().kind);
  for (unsigned i = 0; i < 10; i++) {
    TEST_ASSERT_EQUAL(Kind::BridgeDiscovery, sequence.current().kind);
    sequence.advance();
  }
  TEST_ASSERT_EQUAL(Kind::Health, sequence.current().kind);
  sequence.advance();
  TEST_ASSERT_EQUAL(Kind::Availability, sequence.current().kind);
  TEST_ASSERT_TRUE(sequence.advance());
  TEST_ASSERT_FALSE(sequence.active());
}

void test_stop_aborts_without_completion() {
  Sequence sequence;
  sequence.start(1, 1);
  sequence.stop();

  TEST_ASSERT_FALSE(sequence.active());
  TEST_ASSERT_FALSE(sequence.advance());
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_sequence_emits_one_bounded_step_at_a_time);
  RUN_TEST(test_sequence_skips_empty_remote_and_control_groups);
  RUN_TEST(test_stop_aborts_without_completion);
  return UNITY_END();
}
