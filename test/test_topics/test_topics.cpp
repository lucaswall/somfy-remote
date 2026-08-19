#include <string.h>
#include <unity.h>

#include "topics.h"

void setUp(void) {}
void tearDown(void) {}

static const char *ID = "wemos_somfy_remote";

// Every string below is published by a firmware that is already installed, and Home
// Assistant has twelve devices and thirty-six entities keyed on them. These assertions are
// not describing the code; they are describing an installation the code must not break.
static void topics_match_the_deployed_firmware(void) {
  char buffer[TOPIC_LEN];

  topicCommand(buffer, sizeof(buffer), ID, 3);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/remote3/button", buffer);

  topicCoverState(buffer, sizeof(buffer), ID, 3);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/remote3/state", buffer);

  topicMyState(buffer, sizeof(buffer), ID, 3);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/remote3/my_state", buffer);

  topicCommand(buffer, sizeof(buffer), ID, 11);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/remote11/button", buffer);
}

// Availability is not inherited from anything, so it is the only
// name here that was chosen rather than inherited.
static void availability_hangs_off_the_same_root(void) {
  char buffer[TOPIC_LEN];
  topicAvailability(buffer, sizeof(buffer), ID);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/status", buffer);
}

static void identifiers_match_the_deployed_firmware(void) {
  char buffer[OBJECT_ID_LEN];

  deviceIdentifier(buffer, sizeof(buffer), ID, 0);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote0", buffer);

  uniqueId(buffer, sizeof(buffer), ID, 7, "cover");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote7_cover", buffer);
  uniqueId(buffer, sizeof(buffer), ID, 7, "my");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote7_my", buffer);
  uniqueId(buffer, sizeof(buffer), ID, 7, "prog");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote7_prog", buffer);
}

// cover.somfy_remote4 exists because Home Assistant slugified this exact string. It is
// the single most breakable name in the project.
static void device_name_is_what_the_entity_id_is_made_of(void) {
  char buffer[DEVICE_NAME_LEN];
  deviceName(buffer, sizeof(buffer), 4);
  TEST_ASSERT_EQUAL_STRING("Somfy Remote4", buffer);
  deviceName(buffer, sizeof(buffer), 11);
  TEST_ASSERT_EQUAL_STRING("Somfy Remote11", buffer);
}

static void discovery_topics_match_the_deployed_firmware(void) {
  char object[OBJECT_ID_LEN];
  char buffer[DISCOVERY_TOPIC_LEN];

  uniqueId(object, sizeof(object), ID, 2, "cover");
  discoveryTopic(buffer, sizeof(buffer), "homeassistant/", "cover", object);
  TEST_ASSERT_EQUAL_STRING(
      "homeassistant/cover/wemos_somfy_remote2_cover/config", buffer);

  uniqueId(object, sizeof(object), ID, 2, "my");
  discoveryTopic(buffer, sizeof(buffer), "homeassistant/", "switch", object);
  TEST_ASSERT_EQUAL_STRING("homeassistant/switch/wemos_somfy_remote2_my/config", buffer);

  uniqueId(object, sizeof(object), ID, 2, "prog");
  discoveryTopic(buffer, sizeof(buffer), "homeassistant/", "button", object);
  TEST_ASSERT_EQUAL_STRING("homeassistant/button/wemos_somfy_remote2_prog/config", buffer);
}

// The buffer sizes have to hold the longest name a supported configuration can produce:
// the highest remote index this firmware allows, on a device id long enough to be
// realistic. Truncation here would publish two remotes onto one topic.
static void the_declared_buffers_hold_the_longest_names(void) {
  const char *longId = "a_rather_long_device_identifier";
  char topic[TOPIC_LEN];
  char object[OBJECT_ID_LEN];
  char discovery[DISCOVERY_TOPIC_LEN];

  topicMyState(topic, sizeof(topic), longId, 29);
  TEST_ASSERT_EQUAL_size_t(strlen(longId) + strlen("/remote29/my_state"), strlen(topic));

  uniqueId(object, sizeof(object), longId, 29, "cover");
  TEST_ASSERT_EQUAL_size_t(strlen(longId) + strlen("29_cover"), strlen(object));

  discoveryTopic(discovery, sizeof(discovery), "homeassistant/", "switch", object);
  TEST_ASSERT_EQUAL_size_t(strlen("homeassistant/switch//config") + strlen(object),
                           strlen(discovery));
}

static void subscribes_with_one_wildcard(void) {
  char buffer[TOPIC_LEN];
  topicCommandWildcard(buffer, sizeof(buffer), ID);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/+/button", buffer);
}

// Round trip against the topic we publish for, so the subscription and the parser cannot
// drift apart.
static void parses_back_every_topic_it_builds(void) {
  char topic[TOPIC_LEN];
  for (uint8_t remote = 0; remote < 30; remote++) {
    topicCommand(topic, sizeof(topic), ID, remote);
    uint8_t parsed = 0xFF;
    TEST_ASSERT_TRUE(remoteFromCommandTopic(topic, ID, &parsed));
    TEST_ASSERT_EQUAL_UINT8(remote, parsed);
  }
}

// The wildcard matches every topic one level down, including ones this firmware publishes
// itself. Anything that is not exactly a command topic has to be refused rather than
// parsed into remote 0.
static void refuses_topics_that_are_not_commands(void) {
  uint8_t parsed = 0xFF;
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/remote3/state", ID, &parsed));
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/status", ID, &parsed));
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/remote/button", ID, &parsed));
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/remote3/buttonx", ID, &parsed));
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/remote3x/button", ID, &parsed));
  TEST_ASSERT_FALSE(remoteFromCommandTopic("other_device/remote3/button", ID, &parsed));
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/remote999/button", ID, &parsed));
  TEST_ASSERT_EQUAL_UINT8(0xFF, parsed);
}

// New topics, added alongside the pinned ones rather than in place of them. The existing
// strings above are what Home Assistant keys thirty-six entities on and are not free to
// change; these are additive.
static void new_topics_are_shaped_as_documented(void) {
  char buf[TOPIC_LEN];
  topicConfig(buf, sizeof(buf), "wemos_somfy_remote");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/config", buf);
  topicCode(buf, sizeof(buf), "wemos_somfy_remote", 7);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/code/remote7", buf);
  topicCodeWildcard(buf, sizeof(buf), "wemos_somfy_remote");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/code/+", buf);
  topicNames(buf, sizeof(buf), "wemos_somfy_remote");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/names", buf);
  topicHealth(buf, sizeof(buf), "wemos_somfy_remote");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/health", buf);
}

// A mis-parse here raises the wrong remote's counter, which is unrecoverable in the
// direction that matters, so the parse is exact rather than prefix-based.
static void code_topic_parses_only_its_own_shape(void) {
  uint8_t remote = 0xFF;
  TEST_ASSERT_TRUE(
      remoteFromCodeTopic("wemos_somfy_remote/code/remote11", "wemos_somfy_remote", &remote));
  TEST_ASSERT_EQUAL_UINT8(11, remote);

  TEST_ASSERT_FALSE(
      remoteFromCodeTopic("wemos_somfy_remote/code/remote11/x", "wemos_somfy_remote", &remote));
  TEST_ASSERT_FALSE(
      remoteFromCodeTopic("wemos_somfy_remote/remote11/state", "wemos_somfy_remote", &remote));
  TEST_ASSERT_FALSE(
      remoteFromCodeTopic("wemos_somfy_remote/code/remote", "wemos_somfy_remote", &remote));
  TEST_ASSERT_FALSE(
      remoteFromCodeTopic("other_device/code/remote1", "wemos_somfy_remote", &remote));
}


// --- learned controls -------------------------------------------------------------------

// New topics may be added; the existing ones are not free to change. This one is new, and
// it is pinned from the start so that it joins the set that cannot drift.
static void builds_the_control_topic(void) {
  char topic[TOPIC_LEN];
  topicControl(topic, sizeof(topic), "wemos_somfy_remote", 0x00AABB);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/control/00aabb", topic);

  // Six digits always: an address with leading zeros must not collapse into a shorter
  // topic, or one control would answer to two names.
  topicControl(topic, sizeof(topic), "wemos_somfy_remote", 0x000001);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/control/000001", topic);
}

static void builds_the_control_wildcard(void) {
  char topic[TOPIC_LEN];
  topicControlWildcard(topic, sizeof(topic), "wemos_somfy_remote");
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/control/+", topic);
}

static void builds_the_press_topic(void) {
  char topic[TOPIC_LEN];
  topicControlPress(topic, sizeof(topic), "wemos_somfy_remote", 0x0000FF);
  TEST_ASSERT_EQUAL_STRING("wemos_somfy_remote/control/0000ff/press", topic);
}

static void reads_the_address_back_out_of_the_topic(void) {
  uint32_t address = 0;
  TEST_ASSERT_TRUE(addressFromControlTopic("wemos_somfy_remote/control/00aabb",
                                           "wemos_somfy_remote", &address));
  TEST_ASSERT_EQUAL_HEX32(0x00AABB, address);

  TEST_ASSERT_TRUE(addressFromControlTopic("wemos_somfy_remote/control/000000",
                                           "wemos_somfy_remote", &address));
  TEST_ASSERT_EQUAL_HEX32(0x000000, address);
}

// Every topic this device builds round-trips through its own parser. An address that
// survives the round trip is one that cannot be mis-keyed by a formatting difference.
static void the_control_topic_round_trips(void) {
  static const uint32_t ADDRESSES[5] = {0x000000, 0x000001, 0x0000FF, 0xABCDEF, 0xFFFFFF};
  for (uint8_t i = 0; i < 5; i++) {
    char topic[TOPIC_LEN];
    uint32_t back = 0;
    topicControl(topic, sizeof(topic), "wemos_somfy_remote", ADDRESSES[i]);
    TEST_ASSERT_TRUE(addressFromControlTopic(topic, "wemos_somfy_remote", &back));
    TEST_ASSERT_EQUAL_HEX32(ADDRESSES[i], back);
  }
}

// The subscription is a wildcard, so it matches more than it should. Everything that is not
// exactly six lower-case hex digits has to be refused here, because the thing being keyed
// is the RF address of somebody's motor.
static void refuses_anything_that_is_not_exactly_an_address(void) {
  uint32_t address = 0;
  static const char *const BAD[] = {
      "wemos_somfy_remote/control/00aab",        // five digits
      "wemos_somfy_remote/control/00aabbd",      // seven
      "wemos_somfy_remote/control/00AABB",       // upper case: one address, one topic
      "wemos_somfy_remote/control/00aabg",       // not hex
      "wemos_somfy_remote/control/",             // nothing at all
      "wemos_somfy_remote/control/00aabb/press", // the press topic is not the control
      "wemos_somfy_remote/code/remote0",         // a neighbouring family
      "other_device/control/00aabb",             // somebody else's device id
  };
  for (uint8_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
    TEST_ASSERT_FALSE(addressFromControlTopic(BAD[i], "wemos_somfy_remote", &address));
  }
}

// The control family must not be caught by the parsers that already exist, and vice versa.
// Three wildcards are live on this connection at once and a cross-match moves a shutter.
static void the_control_family_does_not_collide(void) {
  uint8_t remote = 0xFF;
  TEST_ASSERT_FALSE(remoteFromCommandTopic("wemos_somfy_remote/control/00aabb",
                                           "wemos_somfy_remote", &remote));
  TEST_ASSERT_FALSE(remoteFromCodeTopic("wemos_somfy_remote/control/00aabb",
                                        "wemos_somfy_remote", &remote));

  uint32_t address = 0;
  TEST_ASSERT_FALSE(addressFromControlTopic("wemos_somfy_remote/remote3/button",
                                            "wemos_somfy_remote", &address));
  TEST_ASSERT_FALSE(addressFromControlTopic("wemos_somfy_remote/remote3/state",
                                            "wemos_somfy_remote", &address));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(new_topics_are_shaped_as_documented);
  RUN_TEST(code_topic_parses_only_its_own_shape);
  RUN_TEST(topics_match_the_deployed_firmware);
  RUN_TEST(availability_hangs_off_the_same_root);
  RUN_TEST(identifiers_match_the_deployed_firmware);
  RUN_TEST(device_name_is_what_the_entity_id_is_made_of);
  RUN_TEST(discovery_topics_match_the_deployed_firmware);
  RUN_TEST(the_declared_buffers_hold_the_longest_names);
  RUN_TEST(subscribes_with_one_wildcard);
  RUN_TEST(parses_back_every_topic_it_builds);
  RUN_TEST(refuses_topics_that_are_not_commands);
  RUN_TEST(builds_the_control_topic);
  RUN_TEST(builds_the_control_wildcard);
  RUN_TEST(builds_the_press_topic);
  RUN_TEST(reads_the_address_back_out_of_the_topic);
  RUN_TEST(the_control_topic_round_trips);
  RUN_TEST(refuses_anything_that_is_not_exactly_an_address);
  RUN_TEST(the_control_family_does_not_collide);
  return UNITY_END();
}
