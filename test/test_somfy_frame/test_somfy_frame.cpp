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


// --- receive: somfyParseFrame -----------------------------------------------------------

// The inverse of the golden frame above. Parsing is what turns an overheard burst into an
// address, and an address is the only thing that says which control was pressed.
static void parses_a_known_frame(void) {
  const uint8_t frame[SOMFY_FRAME_LEN] = {0xA7, 0x8F, 0x8E, 0xAD, 0xBF, 0x8B, 0xDD};
  SomfyHeard heard;

  TEST_ASSERT_TRUE(somfyParseFrame(frame, &heard));
  TEST_ASSERT_EQUAL_UINT8(SOMFY_UP, heard.command);
  TEST_ASSERT_EQUAL_HEX16(0x0123, heard.rollingCode);
  TEST_ASSERT_EQUAL_HEX32(0x123456, heard.address);
}

// Every frame the builder can produce must parse back to what it was given. This is the
// property the whole receive path rests on, so it is exercised over a spread rather than a
// single case.
static void round_trips_every_built_frame(void) {
  static const SomfyCommand COMMANDS[4] = {SOMFY_MY, SOMFY_UP, SOMFY_DOWN, SOMFY_PROG};
  static const uint16_t CODES[5] = {0x0000, 0x0001, 0x0392, 0x3D2F, 0xFFFF};
  static const uint32_t ADDRESSES[5] = {0x000000, 0x000001, 0x123456, 0xABCDEF, 0xFFFFFF};

  uint8_t frame[SOMFY_FRAME_LEN];
  SomfyHeard heard;

  for (uint8_t c = 0; c < 4; c++) {
    for (uint8_t k = 0; k < 5; k++) {
      for (uint8_t a = 0; a < 5; a++) {
        somfyBuildFrame(COMMANDS[c], CODES[k], ADDRESSES[a], frame);
        TEST_ASSERT_TRUE(somfyParseFrame(frame, &heard));
        TEST_ASSERT_EQUAL_UINT8(COMMANDS[c], heard.command);
        TEST_ASSERT_EQUAL_HEX16(CODES[k], heard.rollingCode);
        TEST_ASSERT_EQUAL_HEX32(ADDRESSES[a], heard.address);
      }
    }
  }
}

// **The checksum cannot see a single-bit error, and this test exists to say so.**
//
// A bit flipped in transmitted byte i unwinds into the same bit flipped in *two* adjacent
// plaintext bytes, because plain[i] = frame[i] ^ frame[i-1] and plain[i+1] = frame[i+1] ^
// frame[i]. The two contributions to a checksum that is just an XOR of nibbles cancel
// exactly, so the frame still sums to zero — while the address, the rolling code or the
// command it decodes to have changed. docs/somfy-rts.md warns that the obfuscation is a
// chain and not a mask; this is the consequence.
//
// Only two of the seven bytes are protected at all: byte 0 by the 0xA7 comparison, and
// byte 6 because nothing follows it to cancel against. 40 of 56 flips survive, and the
// number is asserted exactly so that a future change to the parser cannot quietly make it
// worse without saying so.
//
// This is the measured reason a press is only believed once two copies agree byte for
// byte. A corrupted frame does not look corrupt — it looks like a different remote.
static void cannot_detect_a_single_bit_flip(void) {
  uint8_t good[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_DOWN, 0x1234, 0x765432, good);

  uint16_t accepted = 0;
  uint16_t decodedDifferently = 0;
  for (uint8_t byte = 0; byte < SOMFY_FRAME_LEN; byte++) {
    for (uint8_t bit = 0; bit < 8; bit++) {
      uint8_t bad[SOMFY_FRAME_LEN];
      memcpy(bad, good, SOMFY_FRAME_LEN);
      bad[byte] ^= (uint8_t)(1u << bit);
      SomfyHeard heard;
      if (somfyParseFrame(bad, &heard)) {
        accepted++;
        if (heard.address != 0x765432u || heard.rollingCode != 0x1234 ||
            heard.command != SOMFY_DOWN) {
          decodedDifferently++;
        }
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT16(40, accepted);
  // Every one that got through is wrong about something. None of them is harmless.
  TEST_ASSERT_EQUAL_UINT16(40, decodedDifferently);
}

// The two bytes that are protected, pinned separately so the reason each one is safe stays
// visible: the key comparison for byte 0, and nothing-follows-it for byte 6.
static void catches_a_flip_in_the_key_or_the_last_byte(void) {
  uint8_t good[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_DOWN, 0x1234, 0x765432, good);

  for (uint8_t bit = 0; bit < 8; bit++) {
    uint8_t bad[SOMFY_FRAME_LEN];
    SomfyHeard heard;

    memcpy(bad, good, SOMFY_FRAME_LEN);
    bad[0] ^= (uint8_t)(1u << bit);
    TEST_ASSERT_FALSE(somfyParseFrame(bad, &heard));

    memcpy(bad, good, SOMFY_FRAME_LEN);
    bad[SOMFY_FRAME_LEN - 1] ^= (uint8_t)(1u << bit);
    TEST_ASSERT_FALSE(somfyParseFrame(bad, &heard));
  }
}

// Byte 0 is 0xA7 in every implementation in the wild. It is not a checksum and not a
// secret, but a burst that does not open with it is not an RTS frame, and on a band shared
// with every doorbell and weather station in the street that is worth one comparison.
static void rejects_a_frame_that_does_not_open_with_the_key(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0001, 0x000002, frame);
  frame[0] = 0xA6;

  SomfyHeard heard;
  TEST_ASSERT_FALSE(somfyParseFrame(frame, &heard));
}

// The nine documented commands must all survive, including the five this firmware never
// transmits. A handheld sends them; rejecting an unknown nibble would make a legitimate
// button press look like corruption. See docs/somfy-rts.md.
static void preserves_every_documented_command_nibble(void) {
  static const uint8_t NIBBLES[9] = {0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x8, 0x9, 0xA};

  for (uint8_t i = 0; i < 9; i++) {
    // Built by hand rather than through somfyBuildFrame(), which only knows four of them.
    uint8_t frame[SOMFY_FRAME_LEN];
    frame[0] = SOMFY_KEY;
    frame[1] = (uint8_t)(NIBBLES[i] << 4);
    frame[2] = 0x0A;
    frame[3] = 0x0B;
    frame[4] = 0x0C;
    frame[5] = 0x0D;
    frame[6] = 0x0E;
    uint8_t checksum = 0;
    for (uint8_t b = 0; b < SOMFY_FRAME_LEN; b++) {
      checksum ^= (uint8_t)(frame[b] ^ (frame[b] >> 4));
    }
    frame[1] |= (uint8_t)(checksum & 0x0F);
    for (uint8_t b = 1; b < SOMFY_FRAME_LEN; b++) {
      frame[b] ^= frame[b - 1];
    }

    SomfyHeard heard;
    TEST_ASSERT_TRUE(somfyParseFrame(frame, &heard));
    TEST_ASSERT_EQUAL_UINT8(NIBBLES[i], heard.command);
    TEST_ASSERT_EQUAL_HEX32(0x0C0D0Eu, heard.address);
  }
}

// Noise that survives the timing state machine arrives here as arbitrary bytes. One in
// sixteen passing a four-bit checksum is the number that D-6's two-copy rule exists to
// square; this pins it as a measured property rather than an assumption.
static void random_bytes_pass_at_roughly_the_checksum_rate(void) {
  uint32_t seed = 1u;
  uint16_t accepted = 0;
  const uint16_t trials = 4096;

  for (uint16_t i = 0; i < trials; i++) {
    uint8_t frame[SOMFY_FRAME_LEN];
    frame[0] = SOMFY_KEY;   // the generous case: assume the key byte already matched
    for (uint8_t b = 1; b < SOMFY_FRAME_LEN; b++) {
      seed = seed * 1103515245u + 12345u;
      frame[b] = (uint8_t)(seed >> 16);
    }
    SomfyHeard heard;
    if (somfyParseFrame(frame, &heard)) {
      accepted++;
    }
  }
  // 1/16 of 4096 is 256. Anything far from that means the checksum is not being applied.
  TEST_ASSERT_UINT16_WITHIN(96, 256, accepted);
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
  RUN_TEST(parses_a_known_frame);
  RUN_TEST(round_trips_every_built_frame);
  RUN_TEST(cannot_detect_a_single_bit_flip);
  RUN_TEST(catches_a_flip_in_the_key_or_the_last_byte);
  RUN_TEST(rejects_a_frame_that_does_not_open_with_the_key);
  RUN_TEST(preserves_every_documented_command_nibble);
  RUN_TEST(random_bytes_pass_at_roughly_the_checksum_rate);
  return UNITY_END();
}
