#include <string.h>
#include <unity.h>

#include "somfy_frame.h"

void setUp(void) {}
void tearDown(void) {}

// Golden frames, computed from an independent implementation of the published RTS
// algorithm rather than from the code under test. They are the whole point of this file:
// the receiver is silent, so a frame that is wrong by one nibble looks exactly like a
// shutter that is out of range.
static void builds_a_known_frame(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  const uint8_t want[SOMFY_FRAME_LEN] = {0xA7, 0x8F, 0x8E, 0xAD, 0xBF, 0x8B, 0xDD};

  somfyBuildFrame(SOMFY_UP, 0x0123, 0x123456, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(want, frame, SOMFY_FRAME_LEN);
}

static void builds_the_edges(void) {
  uint8_t frame[SOMFY_FRAME_LEN];

  const uint8_t lowest[SOMFY_FRAME_LEN] = {0xA7, 0xEF, 0xEF, 0xEF, 0xEF, 0xEF, 0xEE};
  somfyBuildFrame(SOMFY_DOWN, 0x0000, 0x000001, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(lowest, frame, SOMFY_FRAME_LEN);

  const uint8_t highest[SOMFY_FRAME_LEN] = {0xA7, 0xBB, 0x44, 0xBB, 0x44, 0xBB, 0x44};
  somfyBuildFrame(SOMFY_MY, 0xFFFF, 0xFFFFFF, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(highest, frame, SOMFY_FRAME_LEN);

  const uint8_t prog[SOMFY_FRAME_LEN] = {0xA7, 0x22, 0x22, 0x23, 0x88, 0x45, 0xAA};
  somfyBuildFrame(SOMFY_PROG, 0x0001, 0xABCDEF, frame);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(prog, frame, SOMFY_FRAME_LEN);
}

// Unwinding the XOR chain and checking the fields is what a receiver does, so doing it
// here proves the frame carries what was asked for rather than merely being stable.
static void round_trips_through_the_obfuscation(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_DOWN, 0x4E20, 0x0A1B2C, frame);

  uint8_t plain[SOMFY_FRAME_LEN];
  plain[0] = frame[0];
  for (uint8_t i = SOMFY_FRAME_LEN - 1; i >= 1; i--) {
    plain[i] = (uint8_t)(frame[i] ^ frame[i - 1]);
  }

  TEST_ASSERT_EQUAL_HEX8(SOMFY_KEY, plain[0]);
  TEST_ASSERT_EQUAL_HEX8(SOMFY_DOWN, plain[1] >> 4);
  TEST_ASSERT_EQUAL_HEX16(0x4E20, (uint16_t)((plain[2] << 8) | plain[3]));
  TEST_ASSERT_EQUAL_HEX32(0x0A1B2CUL,
                          ((uint32_t)plain[4] << 16) | ((uint32_t)plain[5] << 8) | plain[6]);

  // The checksum is a XOR of every nibble including its own, so a correct frame reduces
  // to zero. This is exactly the test the motor applies.
  uint8_t check = 0;
  for (uint8_t i = 0; i < SOMFY_FRAME_LEN; i++) {
    check ^= (uint8_t)(plain[i] ^ (plain[i] >> 4));
  }
  TEST_ASSERT_EQUAL_HEX8(0, check & 0x0F);
}

// A receiver only accepts a code it has not seen, so two presses must not put the same
// bytes on the air. Worth pinning because the checksum can cancel the code's effect
// downstream of the XOR chain: here only two bytes of the frame change, and a weaker
// implementation could easily change none.
static void consecutive_codes_produce_different_frames(void) {
  uint8_t a[SOMFY_FRAME_LEN], b[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0010, 0x123456, a);
  somfyBuildFrame(SOMFY_UP, 0x0011, 0x123456, b);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(a, b, SOMFY_FRAME_LEN));
}

// Consecutive remotes are consecutive addresses, and they share a rolling code counter
// only by coincidence. Two shutters must never see the same frame.
static void neighbouring_remotes_produce_different_frames(void) {
  uint8_t a[SOMFY_FRAME_LEN], b[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0010, 0x123456, a);
  somfyBuildFrame(SOMFY_UP, 0x0010, 0x123457, b);
  TEST_ASSERT_NOT_EQUAL(0, memcmp(a, b, SOMFY_FRAME_LEN));
}

// Every button is a different command nibble, so no two produce the same frame from the
// same remote and code.
static void every_command_produces_a_different_frame(void) {
  const SomfyCommand commands[] = {SOMFY_MY, SOMFY_UP, SOMFY_DOWN, SOMFY_PROG};
  uint8_t frames[4][SOMFY_FRAME_LEN];
  for (uint8_t i = 0; i < 4; i++) {
    somfyBuildFrame(commands[i], 0x0042, 0x123456, frames[i]);
  }
  for (uint8_t i = 0; i < 4; i++) {
    for (uint8_t j = (uint8_t)(i + 1); j < 4; j++) {
      TEST_ASSERT_NOT_EQUAL(0, memcmp(frames[i], frames[j], SOMFY_FRAME_LEN));
    }
  }
}

static void names_every_command(void) {
  TEST_ASSERT_EQUAL_STRING("My", somfyCommandName(SOMFY_MY));
  TEST_ASSERT_EQUAL_STRING("Up", somfyCommandName(SOMFY_UP));
  TEST_ASSERT_EQUAL_STRING("Down", somfyCommandName(SOMFY_DOWN));
  TEST_ASSERT_EQUAL_STRING("Prog", somfyCommandName(SOMFY_PROG));
}

static void parses_payloads_in_any_case(void) {
  const char *texts[] = {"Up", "up", "UP", "uP"};
  for (uint8_t i = 0; i < 4; i++) {
    SomfyCommand command = SOMFY_PROG;
    TEST_ASSERT_TRUE(somfyCommandFromText(texts[i], strlen(texts[i]), &command));
    TEST_ASSERT_EQUAL(SOMFY_UP, command);
  }
}

// The bug this replaces: the old sketch compared only as far as the payload ran, so a
// one-byte "u" opened a shutter and "m" pressed My. A partial payload must be refused,
// not guessed at.
static void refuses_prefixes_and_suffixes(void) {
  SomfyCommand command = SOMFY_PROG;
  TEST_ASSERT_FALSE(somfyCommandFromText("u", 1, &command));
  TEST_ASSERT_FALSE(somfyCommandFromText("m", 1, &command));
  TEST_ASSERT_FALSE(somfyCommandFromText("dow", 3, &command));
  TEST_ASSERT_FALSE(somfyCommandFromText("upward", 6, &command));
  TEST_ASSERT_FALSE(somfyCommandFromText("", 0, &command));
  TEST_ASSERT_FALSE(somfyCommandFromText("stop", 4, &command));
  TEST_ASSERT_EQUAL(SOMFY_PROG, command);   // untouched on every rejection
}

// MQTT payloads are not null-terminated: PubSubClient hands over a pointer into its own
// receive buffer with a length, and whatever follows is the rest of the packet.
static void reads_only_the_given_length(void) {
  SomfyCommand command = SOMFY_PROG;
  TEST_ASSERT_TRUE(somfyCommandFromText("Downstairs", 4, &command));
  TEST_ASSERT_EQUAL(SOMFY_DOWN, command);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(builds_a_known_frame);
  RUN_TEST(builds_the_edges);
  RUN_TEST(round_trips_through_the_obfuscation);
  RUN_TEST(consecutive_codes_produce_different_frames);
  RUN_TEST(neighbouring_remotes_produce_different_frames);
  RUN_TEST(every_command_produces_a_different_frame);
  RUN_TEST(names_every_command);
  RUN_TEST(parses_payloads_in_any_case);
  RUN_TEST(refuses_prefixes_and_suffixes);
  RUN_TEST(reads_only_the_given_length);
  return UNITY_END();
}
