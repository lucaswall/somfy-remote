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

// Availability is the one topic the old firmware never had. It is new, so it is the only
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

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(topics_match_the_deployed_firmware);
  RUN_TEST(availability_hangs_off_the_same_root);
  RUN_TEST(identifiers_match_the_deployed_firmware);
  RUN_TEST(device_name_is_what_the_entity_id_is_made_of);
  RUN_TEST(discovery_topics_match_the_deployed_firmware);
  RUN_TEST(the_declared_buffers_hold_the_longest_names);
  RUN_TEST(subscribes_with_one_wildcard);
  RUN_TEST(parses_back_every_topic_it_builds);
  RUN_TEST(refuses_topics_that_are_not_commands);
  return UNITY_END();
}
