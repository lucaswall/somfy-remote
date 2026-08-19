#include <string.h>
#include <unity.h>

#include "somfy_decoder.h"
#include "somfy_pulses.h"

void setUp(void) {}
void tearDown(void) {}

// A transmission, as the receiver sees it: the pulse train the transmitter would play,
// with runs at the same level merged into one interval — because the radio produces edges,
// not pulses, and two adjacent lows are indistinguishable from one long low on the wire.
//
// This is the whole point of the file. The encoder is already tested against golden frames,
// so driving the decoder from the encoder's own output is the closest thing available to a
// real remote without a radio attached.
#define MAX_INTERVALS 900

struct Interval {
  bool high;
  uint32_t microseconds;
};

static Interval intervals[MAX_INTERVALS];
static size_t intervalCount;

static void addInterval(bool high, uint32_t microseconds) {
  if (intervalCount > 0 && intervals[intervalCount - 1].high == high) {
    intervals[intervalCount - 1].microseconds += microseconds;   // merge, as the air does
    return;
  }
  TEST_ASSERT_LESS_THAN_size_t(MAX_INTERVALS, intervalCount);
  intervals[intervalCount++] = {high, microseconds};
}

static void addFrame(const uint8_t *frame, uint8_t syncCount) {
  SomfyPulse pulses[SOMFY_MAX_PULSES];
  const size_t count = somfyBuildPulses(frame, syncCount, pulses, SOMFY_MAX_PULSES);
  TEST_ASSERT_NOT_EQUAL_size_t(0, count);
  for (size_t i = 0; i < count; i++) {
    addInterval(pulses[i].high, pulses[i].microseconds);
  }
}

// The gaps a real press carries. They are silence, so they merge into whatever low came
// before them — which is exactly the case that has to leave the decoder in a sane state.
static void addGap(uint32_t milliseconds) { addInterval(false, milliseconds * 1000); }

static void beginTransmission(void) { intervalCount = 0; }

// Scales every interval by percent/100, the way a remote whose crystal is not ours would.
static void skew(unsigned percent) {
  for (size_t i = 0; i < intervalCount; i++) {
    intervals[i].microseconds = intervals[i].microseconds * percent / 100;
  }
}

static size_t feedAll(SomfyDecoder *decoder, SomfyHeard *heard, size_t max) {
  size_t decoded = 0;
  for (size_t i = 0; i < intervalCount; i++) {
    SomfyHeard one;
    if (decoder->feed(intervals[i].high, intervals[i].microseconds, &one)) {
      if (decoded < max) {
        heard[decoded] = one;
      }
      decoded++;
    }
  }
  return decoded;
}

// --- the round trip ---------------------------------------------------------------------

static void decodes_a_frame_the_transmitter_built(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0123, 0x123456, frame);

  beginTransmission();
  addGap(80);
  addFrame(frame, SOMFY_FIRST_SYNC);

  SomfyDecoder decoder;
  SomfyHeard heard[4];
  TEST_ASSERT_EQUAL_size_t(1, feedAll(&decoder, heard, 4));
  TEST_ASSERT_EQUAL_UINT8(SOMFY_UP, heard[0].command);
  TEST_ASSERT_EQUAL_HEX16(0x0123, heard[0].rollingCode);
  TEST_ASSERT_EQUAL_HEX32(0x123456, heard[0].address);
}

// Every field must survive, over a spread. A decoder that is right about the address and
// wrong about one bit of the rolling code produces a press that looks perfectly ordinary.
static void round_trips_a_spread_of_frames(void) {
  static const SomfyCommand COMMANDS[4] = {SOMFY_MY, SOMFY_UP, SOMFY_DOWN, SOMFY_PROG};
  static const uint16_t CODES[4] = {0x0000, 0x0001, 0x3D2F, 0xFFFF};
  static const uint32_t ADDRESSES[4] = {0x000000, 0x000001, 0xABCDEF, 0xFFFFFF};

  for (uint8_t c = 0; c < 4; c++) {
    for (uint8_t k = 0; k < 4; k++) {
      for (uint8_t a = 0; a < 4; a++) {
        uint8_t frame[SOMFY_FRAME_LEN];
        somfyBuildFrame(COMMANDS[c], CODES[k], ADDRESSES[a], frame);

        beginTransmission();
        addGap(30);
        addFrame(frame, SOMFY_REPEAT_SYNC);

        SomfyDecoder decoder;
        SomfyHeard heard[2];
        TEST_ASSERT_EQUAL_size_t(1, feedAll(&decoder, heard, 2));
        TEST_ASSERT_EQUAL_UINT8(COMMANDS[c], heard[0].command);
        TEST_ASSERT_EQUAL_HEX16(CODES[k], heard[0].rollingCode);
        TEST_ASSERT_EQUAL_HEX32(ADDRESSES[a], heard[0].address);
      }
    }
  }
}

// The first frame of a press carries two hardware sync pairs and the four repeats carry
// seven. Requiring the larger count would silently discard the first frame of every press —
// which is the one that arrives 150 ms sooner than any other.
static void decodes_a_whole_press_including_the_first_frame(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_DOWN, 0x0392, 0x765432, frame);

  beginTransmission();
  addInterval(true, SOMFY_WAKEUP_HIGH_US);
  addInterval(false, SOMFY_WAKEUP_LOW_US);
  addGap(SOMFY_WAKEUP_GAP_MS);
  addFrame(frame, SOMFY_FIRST_SYNC);
  for (uint8_t i = 0; i < SOMFY_REPEATS; i++) {
    addGap(SOMFY_INTERFRAME_GAP_MS);
    addFrame(frame, SOMFY_REPEAT_SYNC);
  }

  SomfyDecoder decoder;
  SomfyHeard heard[8];
  const size_t decoded = feedAll(&decoder, heard, 8);
  TEST_ASSERT_EQUAL_size_t(1 + SOMFY_REPEATS, decoded);
  for (size_t i = 0; i < decoded; i++) {
    TEST_ASSERT_EQUAL_HEX32(0x765432, heard[i].address);
    TEST_ASSERT_EQUAL_HEX16(0x0392, heard[i].rollingCode);
    TEST_ASSERT_EQUAL_UINT8(SOMFY_DOWN, heard[i].command);
  }
}

// The timings in somfy_pulses.h came from somebody else's remotes. A handheld here may run
// fast or slow, and the tolerance windows exist so that it still decodes.
static void tolerates_a_remote_whose_clock_is_not_ours(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_MY, 0x0011, 0x223344, frame);

  static const unsigned PERCENTS[4] = {80, 90, 110, 120};
  for (uint8_t i = 0; i < 4; i++) {
    beginTransmission();
    addGap(30);
    addFrame(frame, SOMFY_REPEAT_SYNC);
    skew(PERCENTS[i]);

    SomfyDecoder decoder;
    SomfyHeard heard[2];
    TEST_ASSERT_EQUAL_size_t(1, feedAll(&decoder, heard, 2));
    TEST_ASSERT_EQUAL_HEX32(0x223344, heard[0].address);
  }
}

// --- what must not decode ---------------------------------------------------------------

// The band is shared with doorbells, weather stations and car keys. Arbitrary intervals
// must leave the decoder silent rather than confidently wrong.
static void refuses_to_decode_noise(void) {
  SomfyDecoder decoder;
  uint32_t seed = 7u;
  uint16_t frames = 0;

  for (uint16_t i = 0; i < 20000; i++) {
    seed = seed * 1103515245u + 12345u;
    // 450..5570 us, deliberately spanning every window the state machine looks for.
    const uint32_t us = 450 + (seed >> 20) % 5120;
    SomfyHeard heard;
    if (decoder.feed((i & 1) != 0, us, &heard)) {
      frames++;
    }
  }
  TEST_ASSERT_EQUAL_UINT16(0, frames);
}

// Noise immediately before a real press must not stop it being heard: the state machine has
// to be able to fall back into looking for sync from any state it can reach.
static void decodes_a_frame_that_follows_noise(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0002, 0x0000FF, frame);

  SomfyDecoder decoder;
  uint32_t seed = 99u;
  for (uint16_t i = 0; i < 500; i++) {
    seed = seed * 1103515245u + 12345u;
    SomfyHeard ignored;
    decoder.feed((i & 1) != 0, 450 + (seed >> 20) % 5120, &ignored);
  }

  beginTransmission();
  addGap(30);
  addFrame(frame, SOMFY_REPEAT_SYNC);
  SomfyHeard heard[2];
  TEST_ASSERT_EQUAL_size_t(1, feedAll(&decoder, heard, 2));
  TEST_ASSERT_EQUAL_HEX32(0x0000FF, heard[0].address);
}

// A press cut off part-way — the remote moved out of range, or the ring overflowed — must
// produce nothing at all, and must not poison the frame that follows.
static void a_truncated_frame_yields_nothing_and_recovers(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0003, 0x0000AA, frame);

  beginTransmission();
  addGap(30);
  addFrame(frame, SOMFY_REPEAT_SYNC);
  const size_t whole = intervalCount;

  SomfyDecoder decoder;
  SomfyHeard heard;
  for (size_t i = 0; i < whole - 20; i++) {
    TEST_ASSERT_FALSE(decoder.feed(intervals[i].high, intervals[i].microseconds, &heard));
  }

  beginTransmission();
  addGap(30);
  addFrame(frame, SOMFY_REPEAT_SYNC);
  SomfyHeard after[2];
  TEST_ASSERT_EQUAL_size_t(1, feedAll(&decoder, after, 2));
  TEST_ASSERT_EQUAL_HEX32(0x0000AA, after[0].address);
}

// An edge lost mid-frame does NOT slip the bit phase, and that is the uncomfortable part.
//
// The receiver reconstructs intervals by differencing edge timestamps, so a swallowed edge
// merges two adjacent intervals into their sum — 640 + 640 becomes 1280, which is a
// perfectly legal full symbol. The frame runs its whole length, every interval classifies,
// the levels still alternate, and the four-bit checksum cancels the damage often enough that
// a good fraction of merges decode *clean* and *wrong*.
//
// This test exists to keep that fact visible. An earlier version claimed to cover a lost
// edge and actually inserted an extra interval, which shifts the pairing and aborts — it
// passed for a reason unrelated to its name, and the comment above it asserted the opposite
// of what the code did.
static void a_merged_edge_can_decode_clean_and_wrong(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_DOWN, 0x0193, 0x00BCDE, frame);

  beginTransmission();
  addGap(30);
  addFrame(frame, SOMFY_REPEAT_SYNC);
  const size_t whole = intervalCount;

  uint16_t accepted = 0, wrong = 0;
  for (size_t merge = 1; merge + 1 < whole; merge++) {
    SomfyDecoder decoder;
    SomfyHeard heard;
    bool decoded = false;
    for (size_t i = 0; i < whole; i++) {
      if (i == merge) {
        // The lost edge: this interval and the next arrive as one, at the first one's level.
        const uint32_t merged = intervals[i].microseconds + intervals[i + 1].microseconds;
        decoded |= decoder.feed(intervals[i].high, merged, &heard);
        i++;
        continue;
      }
      decoded |= decoder.feed(intervals[i].high, intervals[i].microseconds, &heard);
    }
    if (decoded) {
      accepted++;
      if (heard.address != 0x00BCDEu || heard.rollingCode != 0x0193 ||
          heard.command != SOMFY_DOWN) {
        wrong++;
      }
    }
  }

  // The exact counts depend on the frame, so this asserts the shape rather than a number:
  // merges get through, and the ones that do are mostly lying about which remote pressed
  // what. If either of these ever reads zero, something upstream started catching them and
  // the two-copy rule could be revisited.
  TEST_ASSERT_GREATER_THAN_UINT16(0, accepted);
  TEST_ASSERT_GREATER_THAN_UINT16(0, wrong);
}

// ...which is why a press is only believed once two copies agree. A corrupted copy arriving
// first must not be the one reported.
static void a_corrupted_copy_does_not_win_over_clean_ones(void) {
  SomfyPressAssembler assembler;
  SomfyPress press;

  // Same address and rolling code, different command — the exact shape a merge aligned to
  // the start of byte 1 produces, because the command nibble and the checksum nibble move
  // by the same amount and cancel.
  const SomfyHeard corrupt = {0xB, 0x0193, 0x00BCDE};
  const SomfyHeard clean = {SOMFY_DOWN, 0x0193, 0x00BCDE};

  TEST_ASSERT_FALSE(assembler.feed(corrupt, 1000, &press));
  TEST_ASSERT_FALSE(assembler.feed(clean, 1150, &press));
  TEST_ASSERT_TRUE(assembler.feed(clean, 1300, &press));
  TEST_ASSERT_EQUAL_UINT8(SOMFY_DOWN, press.command);
}

// The ring overflowing mid-frame, which P2.2 asked for and nobody wrote: the decoder must
// give up rather than stitch the two halves into a frame that never existed.
static void a_frame_cut_in_half_by_an_overflow_is_abandoned(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_UP, 0x0005, 0x00CCDD, frame);

  beginTransmission();
  addGap(30);
  addFrame(frame, SOMFY_REPEAT_SYNC);

  SomfyDecoder decoder;
  SomfyHeard heard;
  size_t decoded = 0;
  for (size_t i = 0; i < intervalCount; i++) {
    if (i > intervalCount / 2 && i < intervalCount / 2 + 30) {
      continue;   // thirty intervals that never reached the decoder
    }
    if (decoder.feed(intervals[i].high, intervals[i].microseconds, &heard)) {
      decoded++;
    }
  }
  TEST_ASSERT_EQUAL_size_t(0, decoded);
}

// --- press assembly ---------------------------------------------------------------------

// One press is five identical frames carrying one rolling code. Reporting five presses for
// one button push would make every automation fire five times.
static void five_copies_of_one_press_report_it_once(void) {
  SomfyPressAssembler assembler;
  const SomfyHeard heard = {SOMFY_UP, 0x0100, 0x123456};
  SomfyPress press;

  uint16_t emitted = 0;
  for (uint8_t i = 0; i < 5; i++) {
    if (assembler.feed(heard, 1000 + i * 150, &press)) {
      emitted++;
    }
  }
  TEST_ASSERT_EQUAL_UINT16(1, emitted);
  TEST_ASSERT_EQUAL_HEX32(0x123456, press.address);
  TEST_ASSERT_EQUAL_HEX16(0x0100, press.rollingCode);
  TEST_ASSERT_EQUAL_UINT8(SOMFY_UP, press.command);
}

// A lone frame is not a press. One frame in sixteen of pure noise passes the checksum
// (test_somfy_frame), so a single copy is exactly what must not be believed.
static void one_copy_is_not_a_press(void) {
  SomfyPressAssembler assembler;
  const SomfyHeard heard = {SOMFY_UP, 0x0100, 0x123456};
  SomfyPress press;
  TEST_ASSERT_FALSE(assembler.feed(heard, 1000, &press));
}

// The rolling code is the press identifier: the copies of one press share it and the next
// press carries the next one. Two pushes must read as two.
static void consecutive_presses_are_two_presses(void) {
  SomfyPressAssembler assembler;
  SomfyPress press;
  uint16_t emitted = 0;

  for (uint8_t i = 0; i < 5; i++) {
    const SomfyHeard first = {SOMFY_UP, 0x0100, 0x123456};
    if (assembler.feed(first, 1000 + i * 150, &press)) {
      emitted++;
    }
  }
  for (uint8_t i = 0; i < 5; i++) {
    const SomfyHeard second = {SOMFY_UP, 0x0101, 0x123456};
    if (assembler.feed(second, 3000 + i * 150, &press)) {
      emitted++;
    }
  }
  TEST_ASSERT_EQUAL_UINT16(2, emitted);
  TEST_ASSERT_EQUAL_HEX16(0x0101, press.rollingCode);
}

// A button held down repeats the same code for as long as it is held. That is one press,
// however long the burst runs — the code is what says otherwise.
static void a_held_button_stays_one_press(void) {
  SomfyPressAssembler assembler;
  const SomfyHeard heard = {SOMFY_DOWN, 0x0200, 0x654321};
  SomfyPress press;

  uint16_t emitted = 0;
  for (uint8_t i = 0; i < 40; i++) {
    if (assembler.feed(heard, 5000 + i * 120, &press)) {
      emitted++;
    }
  }
  TEST_ASSERT_EQUAL_UINT16(1, emitted);
}

// Two remotes on the air at once — a neighbour's, or two people during the naming walk —
// must not cancel each other out. A single burst slot would leave both stuck at one copy.
static void two_remotes_at_once_both_report(void) {
  SomfyPressAssembler assembler;
  SomfyPress press;
  uint32_t seenA = 0, seenB = 0;

  for (uint8_t i = 0; i < 5; i++) {
    const SomfyHeard a = {SOMFY_UP, 0x0300, 0x0A0A0A};
    const SomfyHeard b = {SOMFY_DOWN, 0x0400, 0x0B0B0B};
    if (assembler.feed(a, 9000 + i * 150, &press) && press.address == 0x0A0A0Au) {
      seenA++;
    }
    if (assembler.feed(b, 9000 + i * 150, &press) && press.address == 0x0B0B0Bu) {
      seenB++;
    }
  }
  TEST_ASSERT_EQUAL_UINT32(1, seenA);
  TEST_ASSERT_EQUAL_UINT32(1, seenB);
}

// Two copies far enough apart are not one press. Without this a frame heard now and a
// stray one heard a minute later would be assembled into a press that never happened.
static void copies_too_far_apart_are_not_a_press(void) {
  SomfyPressAssembler assembler;
  const SomfyHeard heard = {SOMFY_UP, 0x0500, 0x0C0C0C};
  SomfyPress press;

  TEST_ASSERT_FALSE(assembler.feed(heard, 1000, &press));
  TEST_ASSERT_FALSE(assembler.feed(heard, 1000 + SOMFY_BURST_MS + 1, &press));
}

// The whole chain, end to end: a transmitted press becomes exactly one reported press.
static void a_transmitted_press_becomes_one_reported_press(void) {
  uint8_t frame[SOMFY_FRAME_LEN];
  somfyBuildFrame(SOMFY_DOWN, 0x0392, 0x765432, frame);

  beginTransmission();
  addInterval(true, SOMFY_WAKEUP_HIGH_US);
  addInterval(false, SOMFY_WAKEUP_LOW_US);
  addGap(SOMFY_WAKEUP_GAP_MS);
  addFrame(frame, SOMFY_FIRST_SYNC);
  for (uint8_t i = 0; i < SOMFY_REPEATS; i++) {
    addGap(SOMFY_INTERFRAME_GAP_MS);
    addFrame(frame, SOMFY_REPEAT_SYNC);
  }

  SomfyDecoder decoder;
  SomfyPressAssembler assembler;
  SomfyPress press;
  uint16_t presses = 0;
  uint32_t now = 0;

  for (size_t i = 0; i < intervalCount; i++) {
    now += intervals[i].microseconds / 1000;
    SomfyHeard heard;
    if (decoder.feed(intervals[i].high, intervals[i].microseconds, &heard)) {
      if (assembler.feed(heard, now, &press)) {
        presses++;
      }
    }
  }
  TEST_ASSERT_EQUAL_UINT16(1, presses);
  TEST_ASSERT_EQUAL_HEX32(0x765432, press.address);
  TEST_ASSERT_EQUAL_UINT8(SOMFY_DOWN, press.command);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(decodes_a_frame_the_transmitter_built);
  RUN_TEST(round_trips_a_spread_of_frames);
  RUN_TEST(decodes_a_whole_press_including_the_first_frame);
  RUN_TEST(tolerates_a_remote_whose_clock_is_not_ours);
  RUN_TEST(refuses_to_decode_noise);
  RUN_TEST(decodes_a_frame_that_follows_noise);
  RUN_TEST(a_truncated_frame_yields_nothing_and_recovers);
  RUN_TEST(a_merged_edge_can_decode_clean_and_wrong);
  RUN_TEST(a_corrupted_copy_does_not_win_over_clean_ones);
  RUN_TEST(a_frame_cut_in_half_by_an_overflow_is_abandoned);
  RUN_TEST(five_copies_of_one_press_report_it_once);
  RUN_TEST(one_copy_is_not_a_press);
  RUN_TEST(consecutive_presses_are_two_presses);
  RUN_TEST(a_held_button_stays_one_press);
  RUN_TEST(two_remotes_at_once_both_report);
  RUN_TEST(copies_too_far_apart_are_not_a_press);
  RUN_TEST(a_transmitted_press_becomes_one_reported_press);
  return UNITY_END();
}
