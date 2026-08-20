#include <string.h>
#include <unity.h>

#include "control_map.h"

void setUp(void) {}
void tearDown(void) {}

static ctl::Control make(uint32_t address, const char *name, uint32_t drives) {
  ctl::Control c = {};
  c.address = address;
  c.drives = drives;
  strncpy(c.name, name, ctl::NAME_LEN - 1);
  return c;
}

// --- the payload ------------------------------------------------------------------------

// The address is the topic, not the payload: it is the key, and a key that appears twice is
// a key that can disagree with itself.
static void parses_a_control(void) {
  ctl::Control c;
  const char json[] = "{\"n\":\"Office wall\",\"d\":[0]}";
  TEST_ASSERT_TRUE(ctl::parse(json, strlen(json), 0xAABBCC, &c));
  TEST_ASSERT_EQUAL_HEX32(0xAABBCC, c.address);
  TEST_ASSERT_EQUAL_STRING("Office wall", c.name);
  TEST_ASSERT_EQUAL_HEX32(1u << 0, c.drives);
}

// One control driving several shutters is the normal case, not the exception: a handheld
// channel enrolled at three gallery motors is one press and three covers.
static void parses_a_control_that_drives_several(void) {
  ctl::Control c;
  const char json[] = "{\"n\":\"Gallery ch3\",\"d\":[2,3,4]}";
  TEST_ASSERT_TRUE(ctl::parse(json, strlen(json), 0x010203, &c));
  TEST_ASSERT_EQUAL_HEX32((1u << 2) | (1u << 3) | (1u << 4), c.drives);
}

// A control that drives nothing we emulate — an awning, a gate, a neighbour's motor — is
// still worth naming, because naming it is what stops it coming back as unknown for ever.
static void parses_a_control_that_drives_nothing(void) {
  ctl::Control c;
  const char json[] = "{\"n\":\"Garage\",\"d\":[]}";
  TEST_ASSERT_TRUE(ctl::parse(json, strlen(json), 0x000001, &c));
  TEST_ASSERT_EQUAL_HEX32(0, c.drives);
  TEST_ASSERT_EQUAL_STRING("Garage", c.name);
}

// Only the static bound is enforced here. Whether an index currently exists is a question
// about the configuration document, which arrives *after* these retained topics do — so
// answering it at parse time would empty every drives set on a replacement board.
static void drops_only_indices_beyond_the_static_bound(void) {
  ctl::Control c;
  char json[64];
  snprintf(json, sizeof(json), "{\"n\":\"x\",\"d\":[0,%u,%u,29]}", rs::MAX_REMOTES,
           rs::MAX_REMOTES + 40);
  TEST_ASSERT_TRUE(ctl::parse(json, strlen(json), 0x000002, &c));
  TEST_ASSERT_EQUAL_HEX32((1u << 0) | (1u << 29), c.drives);
}

// Refused, not truncated. Cutting silently once turned three different controls into three
// identical names, which is worse than refusing the save: the map still looks correct and
// nothing downstream can tell the three apart.
static void refuses_an_over_long_name(void) {
  ctl::Control c;
  char json[160];
  char name[ctl::NAME_LEN + 8];
  memset(name, 'W', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  snprintf(json, sizeof(json), "{\"n\":\"%s\",\"d\":[1]}", name);
  TEST_ASSERT_FALSE(ctl::parse(json, strlen(json), 0x000003, &c));
}

// The longest name that does fit must still be accepted, or the limit is off by one.
static void accepts_a_name_of_exactly_the_limit(void) {
  ctl::Control c;
  char json[160];
  char name[ctl::NAME_LEN];
  memset(name, 'W', sizeof(name) - 1);
  name[sizeof(name) - 1] = '\0';
  snprintf(json, sizeof(json), "{\"n\":\"%s\",\"d\":[1]}", name);
  TEST_ASSERT_TRUE(ctl::parse(json, strlen(json), 0x000003, &c));
  TEST_ASSERT_EQUAL_size_t(ctl::NAME_LEN - 1, strlen(c.name));
}

// Long enough for the names a real house needs. 24 was not: three controls in a row cut to
// the same 23 characters and became indistinguishable.
static void the_limit_is_long_enough_to_be_useful(void) {
  TEST_ASSERT_GREATER_OR_EQUAL_UINT8(40, ctl::NAME_LEN);
}

static void refuses_a_payload_that_is_not_json(void) {
  ctl::Control c;
  const char json[] = "Office wall";
  TEST_ASSERT_FALSE(ctl::parse(json, strlen(json), 0x000004, &c));
}

// An empty retained payload is how MQTT says "forget this", and it must not parse into a
// control with an empty name.
static void refuses_an_empty_payload(void) {
  ctl::Control c;
  TEST_ASSERT_FALSE(ctl::parse("", 0, 0x000005, &c));
}

static void round_trips_through_serialise(void) {
  const ctl::Control before = make(0x123456, "Bedroom wall", (1u << 1) | (1u << 7));
  char json[ctl::PAYLOAD_LEN];
  const size_t written = ctl::serialise(before, json, sizeof(json));
  TEST_ASSERT_GREATER_THAN_size_t(0, written);

  ctl::Control after;
  TEST_ASSERT_TRUE(ctl::parse(json, written, before.address, &after));
  TEST_ASSERT_EQUAL_HEX32(before.address, after.address);
  TEST_ASSERT_EQUAL_HEX32(before.drives, after.drives);
  TEST_ASSERT_EQUAL_STRING(before.name, after.name);
}

// The buffer is sized for the worst case: the longest name and every index set. If that
// ever stops fitting, publishing must fail loudly rather than truncate into a payload the
// other end silently rejects.
static void the_worst_case_payload_fits(void) {
  ctl::Control c = make(0xFFFFFF, "", 0xFFFFFFFFu);
  memset(c.name, 'W', ctl::NAME_LEN - 1);
  char json[ctl::PAYLOAD_LEN];
  TEST_ASSERT_GREATER_THAN_size_t(0, ctl::serialise(c, json, sizeof(json)));
}

static void refuses_to_serialise_into_a_short_buffer(void) {
  const ctl::Control c = make(0x000006, "Studio", 1u);
  char json[8];
  TEST_ASSERT_EQUAL_size_t(0, ctl::serialise(c, json, sizeof(json)));
}

// --- the map ----------------------------------------------------------------------------

static void holds_and_finds_controls(void) {
  ctl::ControlMap map;
  TEST_ASSERT_TRUE(map.set(make(0x0A0A0A, "One", 1u)));
  TEST_ASSERT_TRUE(map.set(make(0x0B0B0B, "Two", 2u)));

  TEST_ASSERT_EQUAL_UINT8(2, map.count());
  const ctl::Control *found = map.find(0x0B0B0B);
  TEST_ASSERT_NOT_NULL(found);
  TEST_ASSERT_EQUAL_STRING("Two", found->name);
  TEST_ASSERT_NULL(map.find(0x0C0C0C));
}

// Last write wins per address, because each control is its own retained topic and the
// broker delivers whatever was published last. Two entries for one address would mean the
// same press applied twice.
static void replaces_rather_than_duplicating(void) {
  ctl::ControlMap map;
  TEST_ASSERT_TRUE(map.set(make(0x0A0A0A, "Before", 1u)));
  TEST_ASSERT_TRUE(map.set(make(0x0A0A0A, "After", 4u)));

  TEST_ASSERT_EQUAL_UINT8(1, map.count());
  TEST_ASSERT_EQUAL_STRING("After", map.find(0x0A0A0A)->name);
  TEST_ASSERT_EQUAL_HEX32(4u, map.find(0x0A0A0A)->drives);
}

static void removes_a_control(void) {
  ctl::ControlMap map;
  map.set(make(0x0A0A0A, "One", 1u));
  map.set(make(0x0B0B0B, "Two", 2u));

  TEST_ASSERT_TRUE(map.remove(0x0A0A0A));
  TEST_ASSERT_EQUAL_UINT8(1, map.count());
  TEST_ASSERT_NULL(map.find(0x0A0A0A));
  TEST_ASSERT_NOT_NULL(map.find(0x0B0B0B));
  TEST_ASSERT_FALSE(map.remove(0x0A0A0A));
}

// Full means full: refusing is visible, and silently dropping the thirty-third control a
// person walked across a house to name is not.
static void refuses_to_overflow(void) {
  ctl::ControlMap map;
  for (uint8_t i = 0; i < ctl::MAX_CONTROLS; i++) {
    TEST_ASSERT_TRUE(map.set(make(0x100000u + i, "x", 1u)));
  }
  TEST_ASSERT_EQUAL_UINT8(ctl::MAX_CONTROLS, map.count());
  TEST_ASSERT_FALSE(map.set(make(0x200000u, "one too many", 1u)));

  // ...but replacing one that is already there must still work when full.
  TEST_ASSERT_TRUE(map.set(make(0x100000u, "replaced", 2u)));
  TEST_ASSERT_EQUAL_HEX32(2u, map.find(0x100000u)->drives);
}

// The house is expected to hold thirty-odd addresses: a dozen wall buttons and a couple of
// multi-channel handhelds. A map that could not hold them all would fail in the middle of
// the one walk it exists for.
static void holds_more_than_this_house_needs(void) {
  TEST_ASSERT_GREATER_OR_EQUAL_UINT8(32, ctl::MAX_CONTROLS);
}

void a_name_with_a_quote_or_backslash_is_refused() {
  TEST_ASSERT_FALSE(ctl::nameIsAcceptable("Wall \"Gal\" Left"));
  TEST_ASSERT_FALSE(ctl::nameIsAcceptable("Wall\\Left"));
}

void a_name_with_a_control_character_is_refused() {
  TEST_ASSERT_FALSE(ctl::nameIsAcceptable("Wall\tLeft"));
  TEST_ASSERT_FALSE(ctl::nameIsAcceptable("Wall\nLeft"));   // /log is text/plain: this forges a line
  TEST_ASSERT_FALSE(ctl::nameIsAcceptable(""));
  TEST_ASSERT_FALSE(ctl::nameIsAcceptable(nullptr));
}

// Pinned so a later tightening does not quietly break Spanish names.
void an_apostrophe_and_accented_bytes_are_accepted() {
  TEST_ASSERT_TRUE(ctl::nameIsAcceptable("Mar's Shutters"));
  TEST_ASSERT_TRUE(ctl::nameIsAcceptable("Galer\xc3\xada"));
  TEST_ASSERT_TRUE(ctl::nameIsAcceptable("Balc\xc3\xb3n Izquierdo"));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(parses_a_control);
  RUN_TEST(parses_a_control_that_drives_several);
  RUN_TEST(parses_a_control_that_drives_nothing);
  RUN_TEST(drops_only_indices_beyond_the_static_bound);
  RUN_TEST(refuses_an_over_long_name);
  RUN_TEST(accepts_a_name_of_exactly_the_limit);
  RUN_TEST(the_limit_is_long_enough_to_be_useful);
  RUN_TEST(refuses_a_payload_that_is_not_json);
  RUN_TEST(refuses_an_empty_payload);
  RUN_TEST(round_trips_through_serialise);
  RUN_TEST(the_worst_case_payload_fits);
  RUN_TEST(refuses_to_serialise_into_a_short_buffer);
  RUN_TEST(holds_and_finds_controls);
  RUN_TEST(replaces_rather_than_duplicating);
  RUN_TEST(removes_a_control);
  RUN_TEST(refuses_to_overflow);
  RUN_TEST(holds_more_than_this_house_needs);
  RUN_TEST(a_name_with_a_quote_or_backslash_is_refused);
  RUN_TEST(a_name_with_a_control_character_is_refused);
  RUN_TEST(an_apostrophe_and_accented_bytes_are_accepted);
  return UNITY_END();
}
