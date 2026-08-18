#include <unity.h>

#include "remote_state.h"

void setUp(void) {}
void tearDown(void) {}

// Nothing is known until something is sent. Reporting a guess at boot would be worse than
// saying nothing: the shutter may have been moved by its own handheld remote for weeks.
static void starts_unknown(void) {
  RemoteState state;
  TEST_ASSERT_EQUAL(COVER_UNKNOWN, state.position());
  TEST_ASSERT_EQUAL_UINT32(0, state.version());
}

static void up_and_down_set_the_position(void) {
  RemoteState state;
  state.record(SOMFY_UP);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  state.record(SOMFY_DOWN);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// My stops the shutter somewhere in between and the protocol never says where, so the
// last position we do know stands rather than being replaced by a fresh guess.
static void my_and_prog_leave_the_position_alone(void) {
  RemoteState state;
  state.record(SOMFY_UP);
  state.record(SOMFY_MY);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  state.record(SOMFY_PROG);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  TEST_ASSERT_EQUAL(SOMFY_PROG, state.last());
}

// The version drives publishing, and the My switch has to report itself back off after
// every press. A counter that only moved on a change of position would leave the switch
// stuck on in Home Assistant from the second press onwards.
static void every_command_advances_the_version(void) {
  RemoteState state;
  state.record(SOMFY_MY);
  TEST_ASSERT_EQUAL_UINT32(1, state.version());
  state.record(SOMFY_MY);
  TEST_ASSERT_EQUAL_UINT32(2, state.version());
  state.record(SOMFY_UP);
  state.record(SOMFY_UP);
  TEST_ASSERT_EQUAL_UINT32(4, state.version());
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

static void names_positions_for_the_state_topic(void) {
  TEST_ASSERT_EQUAL_STRING("open", coverPositionName(COVER_OPEN));
  TEST_ASSERT_EQUAL_STRING("closed", coverPositionName(COVER_CLOSED));
  TEST_ASSERT_EQUAL_STRING("unknown", coverPositionName(COVER_UNKNOWN));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(starts_unknown);
  RUN_TEST(up_and_down_set_the_position);
  RUN_TEST(my_and_prog_leave_the_position_alone);
  RUN_TEST(every_command_advances_the_version);
  RUN_TEST(names_positions_for_the_state_topic);
  return UNITY_END();
}
