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
  state.record(SOMFY_UP, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  state.record(SOMFY_DOWN, 100000);
  state.tick(100000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// Prog pairs a remote; it never moves anything, so it must not touch the position.
static void prog_leaves_the_position_alone(void) {
  RemoteState state;
  state.record(SOMFY_UP, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  state.record(SOMFY_PROG, 100000);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  TEST_ASSERT_EQUAL(SOMFY_PROG, state.last());
}

// The version drives publishing, and the My switch has to report itself back off after
// every press. A counter that only moved on a change of position would leave the switch
// stuck on in Home Assistant from the second press onwards.
static void every_command_advances_the_version(void) {
  RemoteState state;
  state.record(SOMFY_MY, 1000);
  TEST_ASSERT_EQUAL_UINT32(1, state.version());
  state.record(SOMFY_MY, 2000);
  TEST_ASSERT_EQUAL_UINT32(2, state.version());
  state.record(SOMFY_UP, 3000);
  state.record(SOMFY_UP, 4000);
  TEST_ASSERT_EQUAL_UINT32(4, state.version());
}

static void names_positions_for_the_state_topic(void) {
  TEST_ASSERT_EQUAL_STRING("open", coverPositionName(COVER_OPEN));
  TEST_ASSERT_EQUAL_STRING("closed", coverPositionName(COVER_CLOSED));
  TEST_ASSERT_EQUAL_STRING("opening", coverPositionName(COVER_OPENING));
  TEST_ASSERT_EQUAL_STRING("closing", coverPositionName(COVER_CLOSING));
  TEST_ASSERT_EQUAL_STRING("unknown", coverPositionName(COVER_UNKNOWN));
}


// --- overheard presses ------------------------------------------------------------------

// The whole point of receiving: a shutter opened by hand stops reading closed.
static void an_overheard_press_moves_the_position(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  state.observe(SOMFY_DOWN, 100000);
  state.tick(100000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// Nothing publishes until the version moves, so an observation that does not advance it is
// an observation Home Assistant never hears about.
static void an_overheard_press_advances_the_version(void) {
  RemoteState state;
  const uint32_t before = state.version();
  state.observe(SOMFY_UP, 1000);
  TEST_ASSERT_EQUAL_UINT32(before + 1, state.version());
  state.observe(SOMFY_UP, 2000);
  TEST_ASSERT_EQUAL_UINT32(before + 2, state.version());
}

// A handheld sends five commands this firmware never transmits. They move the shutter in
// ways no position can be computed from, so the honest answer is to leave the last one
// standing rather than to guess.
static void an_unknown_command_leaves_the_position_alone(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  static const uint8_t OTHERS[5] = {0x3, 0x5, 0x6, 0x9, 0xA};
  for (uint8_t i = 0; i < 5; i++) {
    state.observe(OTHERS[i], 100000);
    TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  }
}


// --- restore ----------------------------------------------------------------------------

// restore() runs on every reconnect, for every remote, from the retained cover state. Its
// whole behaviour is one guard — and without it every cover would be rewound to whatever the
// broker last retained each time the connection blinked, silently, with the rest of the
// suite still green. It had no test at all.
static void restore_only_fills_an_unknown_position(void) {
  RemoteState state;
  state.restore(COVER_OPEN);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

static void restore_never_overwrites_what_we_did(void) {
  RemoteState state;
  state.record(SOMFY_DOWN, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  state.restore(COVER_OPEN);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());

  // Nor what we overheard somebody else do, nor a travel still in progress.
  RemoteState heard;
  heard.observe(SOMFY_UP, 1000);
  heard.restore(COVER_CLOSED);
  TEST_ASSERT_EQUAL(COVER_OPENING, heard.position());
}

// Nothing was transmitted, so no press may be implied — otherwise a reconnect would look
// like activity and republish state that nobody caused.
static void restore_does_not_advance_the_version(void) {
  RemoteState state;
  const uint32_t before = state.version();
  state.restore(COVER_OPEN);
  TEST_ASSERT_EQUAL_UINT32(before, state.version());
}


// --- travel ------------------------------------------------------------------------------

static void up_reports_opening_then_open(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  TEST_ASSERT_EQUAL(COVER_OPENING, state.position());

  state.tick(1000 + COVER_TRAVEL_MS - 1);
  TEST_ASSERT_EQUAL(COVER_OPENING, state.position());

  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

static void down_reports_closing_then_closed(void) {
  RemoteState state;
  state.observe(SOMFY_DOWN, 1000);
  TEST_ASSERT_EQUAL(COVER_CLOSING, state.position());
  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// Arriving is a change somebody has to hear about, or the cover animates open for ever.
static void arriving_advances_the_version(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  const uint32_t moving = state.version();
  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL_UINT32(moving + 1, state.version());

  // ...and only once.
  state.tick(1000 + COVER_TRAVEL_MS + 5000);
  TEST_ASSERT_EQUAL_UINT32(moving + 1, state.version());
}

// My mid-travel stops the motor. A cover with no position reports anything not fully shut as
// open, so a shutter stopped half way is open — which is also what is_closed must say.
static void my_while_moving_stops_short_of_closed(void) {
  RemoteState state;
  state.observe(SOMFY_DOWN, 1000);
  state.tick(1000 + COVER_TRAVEL_MS / 2);
  state.observe(SOMFY_MY, 1000 + COVER_TRAVEL_MS / 2);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());

  // And the travel it interrupted must not still land later.
  state.tick(1000 + COVER_TRAVEL_MS * 2);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

// Every motor in this installation has its favourite position set to fully closed, so My on
// a shutter that is not moving drives it shut. That is a fact about these motors rather than
// about the protocol — elsewhere the favourite is usually somewhere in the middle.
static void my_while_stopped_closes(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());

  state.observe(SOMFY_MY, 50000);
  TEST_ASSERT_EQUAL(COVER_CLOSING, state.position());
  state.tick(50000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// Reversing half way down is half a journey back, not a whole one: the clock is set from
// the distance left to cover, not from the length of the shutter.
static void reversing_mid_travel_measures_the_way_back(void) {
  RemoteState state;
  const uint32_t turned = 1000 + COVER_TRAVEL_MS / 2;
  state.observe(SOMFY_DOWN, 1000);
  state.observe(SOMFY_UP, turned);
  TEST_ASSERT_EQUAL(COVER_OPENING, state.position());
  TEST_ASSERT_EQUAL_INT16(50, state.percent(turned));

  state.tick(turned + COVER_TRAVEL_MS / 2 - 1);
  TEST_ASSERT_EQUAL(COVER_OPENING, state.position());
  state.tick(turned + COVER_TRAVEL_MS / 2);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

// Our own commands travel exactly as overheard ones do; the shutter cannot tell who asked.
static void our_own_commands_travel_too(void) {
  RemoteState state;
  state.record(SOMFY_DOWN, 1000);
  TEST_ASSERT_EQUAL(COVER_CLOSING, state.position());
  state.tick(1000 + COVER_TRAVEL_MS);
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

// millis() wraps every 49 days and a shutter must not be left travelling until it does.
static void travel_survives_the_rollover(void) {
  RemoteState state;
  const uint32_t nearTheEnd = 0xFFFFFFFFu - (COVER_TRAVEL_MS / 2);
  state.observe(SOMFY_UP, nearTheEnd);
  TEST_ASSERT_EQUAL(COVER_OPENING, state.position());
  state.tick((uint32_t)(nearTheEnd + COVER_TRAVEL_MS));
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

// The travel time is a measurement of these shutters, not a guess.
static void travel_time_matches_the_installation(void) {
  TEST_ASSERT_EQUAL_UINT32(22000, COVER_TRAVEL_MS);
}


// --- the percentage -----------------------------------------------------------------------

static void percent_is_unknown_until_something_moves(void) {
  RemoteState state;
  TEST_ASSERT_EQUAL_INT16(COVER_PCT_UNKNOWN, state.percent(1000));
}

static void percent_follows_the_travel(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  TEST_ASSERT_EQUAL_INT16(0, state.percent(1000));
  TEST_ASSERT_EQUAL_INT16(50, state.percent(1000 + COVER_TRAVEL_MS / 2));
  TEST_ASSERT_EQUAL_INT16(100, state.percent(1000 + COVER_TRAVEL_MS));

  state.tick(1000 + COVER_TRAVEL_MS);
  state.observe(SOMFY_DOWN, 100000);
  TEST_ASSERT_EQUAL_INT16(100, state.percent(100000));
  TEST_ASSERT_EQUAL_INT16(75, state.percent(100000 + COVER_TRAVEL_MS / 4));
  TEST_ASSERT_EQUAL_INT16(0, state.percent(100000 + COVER_TRAVEL_MS));
}

// The point of tracking a percentage rather than a direction: a shutter stopped half way
// stays half way, and the next command starts from there.
static void my_freezes_the_percentage(void) {
  RemoteState state;
  state.observe(SOMFY_DOWN, 1000);
  state.observe(SOMFY_MY, 1000 + COVER_TRAVEL_MS / 4);
  TEST_ASSERT_EQUAL_INT16(75, state.percent(1000 + COVER_TRAVEL_MS / 4));
  TEST_ASSERT_EQUAL_INT16(75, state.percent(500000));
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

// A quarter of the way down takes a quarter of the time to put back, not the whole of it.
static void a_short_journey_takes_a_short_time(void) {
  RemoteState state;
  state.observe(SOMFY_DOWN, 1000);
  state.observe(SOMFY_MY, 1000 + COVER_TRAVEL_MS / 4);   // stopped at 75 %
  state.observe(SOMFY_UP, 100000);

  state.tick(100000 + COVER_TRAVEL_MS / 4);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
  TEST_ASSERT_EQUAL_INT16(100, state.percent(100000 + COVER_TRAVEL_MS / 4));
}

// Pressing Up on a shutter that is already open must not leave it reading `opening` for
// twenty-two seconds; there is nowhere for it to travel to.
static void a_command_that_changes_nothing_does_not_travel(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  state.tick(1000 + COVER_TRAVEL_MS);
  state.observe(SOMFY_UP, 100000);
  TEST_ASSERT_FALSE(state.travelling());
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

static void each_shutter_carries_its_own_travel_time(void) {
  RemoteState state;
  state.setTravelMs(10000);
  TEST_ASSERT_EQUAL_UINT32(10000, state.travelMs());

  state.observe(SOMFY_UP, 1000);
  TEST_ASSERT_EQUAL_INT16(50, state.percent(6000));
  state.tick(11000);
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

// Zero would make every travel instantaneous and divide the interpolation by nothing.
static void a_travel_time_of_zero_is_refused(void) {
  RemoteState state;
  state.setTravelMs(0);
  TEST_ASSERT_EQUAL_UINT32(COVER_TRAVEL_MS, state.travelMs());
}

// The retained state topic says `open` for a shutter stopped anywhere short of shut, so
// without the retained percentage a reboot rounds every partial position up to fully open.
static void restore_takes_the_finer_answer(void) {
  RemoteState state;
  state.restore(COVER_OPEN);
  state.restorePercent(40);
  TEST_ASSERT_EQUAL_INT16(40, state.percent(1000));
  TEST_ASSERT_EQUAL(COVER_OPEN, state.position());
}

static void restore_never_contradicts_a_closed_shutter(void) {
  RemoteState state;
  state.restore(COVER_CLOSED);
  state.restorePercent(90);
  TEST_ASSERT_EQUAL_INT16(0, state.percent(1000));
  TEST_ASSERT_EQUAL(COVER_CLOSED, state.position());
}

static void restore_never_interrupts_a_travel(void) {
  RemoteState state;
  state.observe(SOMFY_UP, 1000);
  state.restorePercent(5);
  TEST_ASSERT_EQUAL_INT16(50, state.percent(1000 + COVER_TRAVEL_MS / 2));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(starts_unknown);
  RUN_TEST(up_and_down_set_the_position);
  RUN_TEST(prog_leaves_the_position_alone);
  RUN_TEST(every_command_advances_the_version);
  RUN_TEST(names_positions_for_the_state_topic);
  RUN_TEST(an_overheard_press_moves_the_position);
  RUN_TEST(an_overheard_press_advances_the_version);
  RUN_TEST(an_unknown_command_leaves_the_position_alone);
  RUN_TEST(restore_only_fills_an_unknown_position);
  RUN_TEST(restore_never_overwrites_what_we_did);
  RUN_TEST(restore_does_not_advance_the_version);
  RUN_TEST(up_reports_opening_then_open);
  RUN_TEST(down_reports_closing_then_closed);
  RUN_TEST(arriving_advances_the_version);
  RUN_TEST(my_while_moving_stops_short_of_closed);
  RUN_TEST(my_while_stopped_closes);
  RUN_TEST(reversing_mid_travel_measures_the_way_back);
  RUN_TEST(our_own_commands_travel_too);
  RUN_TEST(travel_survives_the_rollover);
  RUN_TEST(travel_time_matches_the_installation);
  RUN_TEST(percent_is_unknown_until_something_moves);
  RUN_TEST(percent_follows_the_travel);
  RUN_TEST(my_freezes_the_percentage);
  RUN_TEST(a_short_journey_takes_a_short_time);
  RUN_TEST(a_command_that_changes_nothing_does_not_travel);
  RUN_TEST(each_shutter_carries_its_own_travel_time);
  RUN_TEST(a_travel_time_of_zero_is_refused);
  RUN_TEST(restore_takes_the_finer_answer);
  RUN_TEST(restore_never_contradicts_a_closed_shutter);
  RUN_TEST(restore_never_interrupts_a_travel);
  return UNITY_END();
}
