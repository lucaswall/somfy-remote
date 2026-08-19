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


// --- overheard presses ------------------------------------------------------------------

// The whole point of receiving: a shutter opened by hand stops reading closed.
static void an_overheard_press_moves_the_position(void) {
  RemoteState state;
  state.observe(SOMFY_UP);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  state.observe(SOMFY_DOWN);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// Nothing publishes until the version moves, so an observation that does not advance it is
// an observation Home Assistant never hears about.
static void an_overheard_press_advances_the_version(void) {
  RemoteState state;
  const uint32_t before = state.version();
  state.observe(SOMFY_UP);
  TEST_ASSERT_EQUAL_UINT32(before + 1, state.version());
  state.observe(SOMFY_UP);
  TEST_ASSERT_EQUAL_UINT32(before + 2, state.version());
}

// A handheld sends five commands this firmware never transmits. They move the shutter in
// ways no position can be computed from, so the honest answer is to leave the last one
// standing rather than to guess.
static void an_unknown_command_leaves_the_position_alone(void) {
  RemoteState state;
  state.observe(SOMFY_UP);
  static const uint8_t OTHERS[5] = {0x3, 0x5, 0x6, 0x9, 0xA};
  for (uint8_t i = 0; i < 5; i++) {
    state.observe(OTHERS[i]);
    TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  }
}

// My stops the shutter wherever it happens to be. Nothing in the protocol says where that
// is, and inventing an answer is worse than admitting there is none.
static void an_overheard_my_does_not_invent_a_position(void) {
  RemoteState state;
  state.observe(SOMFY_MY);
  TEST_ASSERT_EQUAL(COVER_UNKNOWN, state.position());
  TEST_ASSERT_EQUAL(SOMFY_MY, state.last());

  state.observe(SOMFY_DOWN);
  state.observe(SOMFY_MY);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(starts_unknown);
  RUN_TEST(up_and_down_set_the_position);
  RUN_TEST(my_and_prog_leave_the_position_alone);
  RUN_TEST(every_command_advances_the_version);
  RUN_TEST(names_positions_for_the_state_topic);
  RUN_TEST(an_overheard_press_moves_the_position);
  RUN_TEST(an_overheard_press_advances_the_version);
  RUN_TEST(an_unknown_command_leaves_the_position_alone);
  RUN_TEST(an_overheard_my_does_not_invent_a_position);
  return UNITY_END();
}
