#include <string.h>
#include <unity.h>

#include "record_store.h"

using namespace rs;

static uint8_t sector[SECTOR_SIZE];

static void erase() { memset(sector, 0xFF, sizeof(sector)); }
static uint8_t *slot(uint16_t n) { return sector + (uint32_t)n * RECORD_SIZE; }

static void writeAt(uint16_t n, uint8_t ns, uint8_t id, uint32_t value) {
  encode(slot(n), ns, id, value);
}

void setUp() { erase(); }
void tearDown() {}

// --- record format ---------------------------------------------------------------------

void round_trips_a_record() {
  writeAt(3, NS_CODE, 7, 0xDEADBEEF);
  TEST_ASSERT_EQUAL(SLOT_VALID, classify(slot(3)));
  TEST_ASSERT_EQUAL_UINT8(NS_CODE, slot(3)[0]);
  TEST_ASSERT_EQUAL_UINT8(7, slot(3)[1]);
  TEST_ASSERT_EQUAL_UINT32(0xDEADBEEF, valueOf(slot(3)));
}

void erased_slot_is_free() { TEST_ASSERT_EQUAL(SLOT_FREE, classify(slot(0))); }

void corrupted_crc_is_spent() {
  writeAt(1, NS_CODE, 0, 100);
  slot(1)[7] ^= 0x01;
  TEST_ASSERT_EQUAL(SLOT_SPENT, classify(slot(1)));
}

void nonzero_flags_is_spent() {
  writeAt(1, NS_CODE, 0, 100);
  slot(1)[6] = 0x01;   // CRC no longer matches either, but flags alone must disqualify
  TEST_ASSERT_EQUAL(SLOT_SPENT, classify(slot(1)));
}

// A slot that failed to program can read back as all zeros. With a zero CRC init that
// would compute a CRC of 0x00 and pass as a valid CODE[0] = 0 record, silently resetting
// remote 0's counter. The 0xFF init is what makes it fail closed.
void all_zero_slot_is_spent_not_a_valid_zero_counter() {
  memset(slot(1), 0x00, RECORD_SIZE);
  TEST_ASSERT_EQUAL(SLOT_SPENT, classify(slot(1)));
}

void partially_programmed_slot_is_spent() {
  writeAt(1, NS_CODE, 4, 999);
  slot(1)[5] = 0xFF;   // a byte that never got programmed
  TEST_ASSERT_EQUAL(SLOT_SPENT, classify(slot(1)));
}

// --- replay ----------------------------------------------------------------------------

void last_write_wins() {
  writeAt(1, NS_CODE, 2, 10);
  writeAt(2, NS_CODE, 2, 11);
  writeAt(3, NS_CODE, 2, 12);
  LiveMap map;
  replay(sector, &map);
  TEST_ASSERT_EQUAL_UINT32(12, map.valueOr(NS_CODE, 2, 0));
}

void replay_skips_spent_and_counts_it() {
  writeAt(1, NS_CODE, 0, 100);
  writeAt(2, NS_CODE, 0, 101);
  slot(2)[7] ^= 0xFF;   // torn
  writeAt(3, NS_CODE, 0, 102);
  LiveMap map;
  const ReplayResult r = replay(sector, &map);
  TEST_ASSERT_EQUAL_UINT16(1, r.spent);
  TEST_ASSERT_EQUAL_UINT32(102, map.valueOr(NS_CODE, 0, 0));
}

// A torn record sitting before free space is exactly where "first free slot" and "one
// past the highest used slot" diverge. Taking the first free slot would re-program an
// occupied one.
void append_pointer_is_past_the_highest_used_slot_not_the_first_free_one() {
  writeAt(1, NS_CODE, 0, 100);
  memset(slot(2), 0x00, RECORD_SIZE);   // spent, not free
  writeAt(3, NS_CODE, 0, 101);
  LiveMap map;
  const ReplayResult r = replay(sector, &map);
  TEST_ASSERT_EQUAL_UINT16(4, r.appendSlot);
}

void replay_of_empty_sector_starts_at_slot_one() {
  LiveMap map;
  const ReplayResult r = replay(sector, &map);
  TEST_ASSERT_EQUAL_UINT16(1, r.appendSlot);
  TEST_ASSERT_EQUAL_UINT8(0, map.count());
}

// A firmware newer than this one may write namespaces this one does not know. Compaction
// must carry them forward, or a downgrade destroys them.
void unknown_namespaces_survive_replay_and_compaction() {
  writeAt(1, NS_CODE, 0, 5);
  writeAt(2, 0x42, 9, 0xABCD);
  LiveMap map;
  replay(sector, &map);
  TEST_ASSERT_EQUAL_UINT32(0xABCD, map.valueOr(0x42, 9, 0));

  uint8_t body[SECTOR_SIZE];
  const uint16_t n = snapshotBody(map, body, SLOTS - 1);
  TEST_ASSERT_EQUAL_UINT16(2, n);

  uint8_t rebuilt[SECTOR_SIZE];
  memset(rebuilt, 0xFF, sizeof(rebuilt));
  memcpy(rebuilt + RECORD_SIZE, body, (uint32_t)n * RECORD_SIZE);
  LiveMap after;
  replay(rebuilt, &after);
  TEST_ASSERT_EQUAL_UINT32(0xABCD, after.valueOr(0x42, 9, 0));
}

void a_header_outside_slot_zero_is_not_a_live_value() {
  writeAt(5, NS_HEADER, 0, 77);
  LiveMap map;
  replay(sector, &map);
  TEST_ASSERT_FALSE(map.has(NS_HEADER, 0));
}

// --- sector state ----------------------------------------------------------------------

void blank_sector_has_no_header() {
  uint32_t gen;
  TEST_ASSERT_FALSE(sectorHeader(sector, &gen));
}

void valid_header_reads_its_generation() {
  writeAt(0, NS_HEADER, 0, 9);
  uint32_t gen = 0;
  TEST_ASSERT_TRUE(sectorHeader(sector, &gen));
  TEST_ASSERT_EQUAL_UINT32(9, gen);
}

// The installed board's legacy sector begins 2F 3D 6F 6B ... — neither erased nor a valid
// header. A migration keyed on "blank" could never have fired on the one device that
// needed it.
void legacy_counter_bytes_in_slot_zero_are_foreign_not_blank() {
  const uint8_t legacy[8] = {0x2F, 0x3D, 0x6F, 0x6B, 0x92, 0x03, 0x8F, 0x03};
  memcpy(slot(0), legacy, sizeof(legacy));
  uint32_t gen;
  TEST_ASSERT_FALSE(sectorHeader(sector, &gen));
  TEST_ASSERT_NOT_EQUAL(SLOT_FREE, classify(slot(0)));
}

void torn_header_falls_back_to_the_other_sector() {
  TEST_ASSERT_EQUAL(ACTIVE_A, selectActive(true, 4, false, 0));
  TEST_ASSERT_EQUAL(ACTIVE_B, selectActive(false, 0, true, 4));
  TEST_ASSERT_EQUAL(ACTIVE_NONE, selectActive(false, 0, false, 0));
}

void higher_generation_wins() {
  TEST_ASSERT_EQUAL(ACTIVE_B, selectActive(true, 4, true, 5));
  TEST_ASSERT_EQUAL(ACTIVE_A, selectActive(true, 6, true, 5));
}

// --- capacity --------------------------------------------------------------------------

void live_set_at_thirty_remotes_leaves_room_to_append() {
  LiveMap map;
  for (uint8_t i = 0; i < MAX_REMOTES; i++) {
    map.put(NS_CODE, i, 1);
    map.put(NS_ADDR, i, 0x100000 + i);
    map.put(NS_FLAGS, i, FLAG_ENABLED | FLAG_OPERATIONAL);
  }
  map.put(NS_SCALAR, SCALAR_REMOTE_COUNT, MAX_REMOTES);
  map.put(NS_SCALAR, SCALAR_ADDRESS_BASE, 0x100000);
  map.put(NS_SCALAR, SCALAR_CONFIG_EPOCH, 1);
  map.put(NS_SCALAR, SCALAR_LEGACY_RELEASED, 1);
  TEST_ASSERT_EQUAL_UINT8(94, map.count());
  // 512 slots, one header, 94 live records: the rest is append space per rotation.
  TEST_ASSERT_TRUE(SLOTS - 1 - map.count() > 400);
}

void live_map_reports_overflow_rather_than_corrupting() {
  LiveMap map;
  for (uint16_t i = 0; i < MAX_ENTRIES; i++) {
    TEST_ASSERT_TRUE(map.put(0x10, (uint8_t)i, i));
  }
  TEST_ASSERT_FALSE(map.put(0x11, 0, 1));
}

// --- counters --------------------------------------------------------------------------

void transmitted_code_is_the_low_sixteen_bits() {
  TEST_ASSERT_EQUAL_UINT16(0, transmitCode(0x10000));
  TEST_ASSERT_EQUAL_UINT16(0xFFFF, transmitCode(0xFFFF));
  TEST_ASSERT_EQUAL_UINT16(1, transmitCode(0x10001));
}

// The counter is monotonic u32 precisely so this stays a total order. Over a wrapping
// u16, max(0x0000, 0xFFFF) would re-select an already-transmitted code.
void counter_keeps_advancing_across_the_sixteen_bit_wrap() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_KEEP_QUIET, reconcile(true, 0xFFFF, true, 0xFFFF, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(0xFFFF, effective);
  TEST_ASSERT_EQUAL(REC_ADOPT, reconcile(true, 0xFFFF, true, 0x10000, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(0x10000, effective);
  TEST_ASSERT_EQUAL_UINT16(0, transmitCode(effective));
}

// --- reconciliation --------------------------------------------------------------------

void mirror_ahead_is_adopted() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_ADOPT, reconcile(true, 100, true, 150, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(150, effective);
}

void mirror_behind_keeps_local_and_publishes() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_KEEP_PUBLISH, reconcile(true, 150, true, 100, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(150, effective);
}

// The case that would have destroyed the mirror: a blank board has local 0 and no mirror
// entry yet. Treating "absent" as "local won" would publish 0 over a retained 914.
void absent_mirror_never_triggers_a_publish() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_KEEP_QUIET, reconcile(true, 0, false, 0, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(0, effective);
}

void blank_board_adopts_the_mirror() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_ADOPT, reconcile(false, 0, true, 914, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(914, effective);
}

void neither_side_means_the_remote_cannot_transmit() {
  uint32_t effective = 0xAA;
  TEST_ASSERT_EQUAL(REC_NONE, reconcile(false, 0, false, 0, 1000, &effective));
}

void equal_values_publish_nothing() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_KEEP_QUIET, reconcile(true, 500, true, 500, 1000, &effective));
}

// One mistyped mosquitto_pub can otherwise span the counter space, and unlike a stale
// mirror this is not self-correcting.
void an_implausible_forward_jump_is_refused() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_REFUSE_JUMP, reconcile(true, 100, true, 5000000, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(100, effective);
}

void a_jump_at_the_limit_is_still_adopted() {
  uint32_t effective = 0;
  TEST_ASSERT_EQUAL(REC_ADOPT, reconcile(true, 100, true, 1100, 1000, &effective));
  TEST_ASSERT_EQUAL_UINT32(1100, effective);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(round_trips_a_record);
  RUN_TEST(erased_slot_is_free);
  RUN_TEST(corrupted_crc_is_spent);
  RUN_TEST(nonzero_flags_is_spent);
  RUN_TEST(all_zero_slot_is_spent_not_a_valid_zero_counter);
  RUN_TEST(partially_programmed_slot_is_spent);
  RUN_TEST(last_write_wins);
  RUN_TEST(replay_skips_spent_and_counts_it);
  RUN_TEST(append_pointer_is_past_the_highest_used_slot_not_the_first_free_one);
  RUN_TEST(replay_of_empty_sector_starts_at_slot_one);
  RUN_TEST(unknown_namespaces_survive_replay_and_compaction);
  RUN_TEST(a_header_outside_slot_zero_is_not_a_live_value);
  RUN_TEST(blank_sector_has_no_header);
  RUN_TEST(valid_header_reads_its_generation);
  RUN_TEST(legacy_counter_bytes_in_slot_zero_are_foreign_not_blank);
  RUN_TEST(torn_header_falls_back_to_the_other_sector);
  RUN_TEST(higher_generation_wins);
  RUN_TEST(live_set_at_thirty_remotes_leaves_room_to_append);
  RUN_TEST(live_map_reports_overflow_rather_than_corrupting);
  RUN_TEST(transmitted_code_is_the_low_sixteen_bits);
  RUN_TEST(counter_keeps_advancing_across_the_sixteen_bit_wrap);
  RUN_TEST(mirror_ahead_is_adopted);
  RUN_TEST(mirror_behind_keeps_local_and_publishes);
  RUN_TEST(absent_mirror_never_triggers_a_publish);
  RUN_TEST(blank_board_adopts_the_mirror);
  RUN_TEST(neither_side_means_the_remote_cannot_transmit);
  RUN_TEST(equal_values_publish_nothing);
  RUN_TEST(an_implausible_forward_jump_is_refused);
  RUN_TEST(a_jump_at_the_limit_is_still_adopted);
  return UNITY_END();
}
