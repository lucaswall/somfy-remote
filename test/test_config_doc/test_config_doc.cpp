#include <string.h>
#include <unity.h>

#include "config_doc.h"

using namespace cfg;

void setUp() {}
void tearDown() {}

static const char SAMPLE[] =
    "{\"v\":1,\"epoch\":7,\"writer\":\"ui\",\"base\":\"0x000000\",\"remotes\":["
    "{\"i\":0,\"enabled\":true,\"operational\":true},"
    "{\"i\":1,\"addr\":\"0x000105\",\"enabled\":true,\"operational\":false}]}";

void parses_a_document() {
  ConfigDoc doc;
  TEST_ASSERT_TRUE(parse(SAMPLE, strlen(SAMPLE), &doc));
  TEST_ASSERT_EQUAL_UINT32(7, doc.epoch);
  TEST_ASSERT_EQUAL_UINT8(2, doc.entries);
  TEST_ASSERT_EQUAL_STRING("ui", doc.writer);
  TEST_ASSERT_FALSE(doc.remotes[1].operational);
}

void a_remote_without_an_address_derives_it_from_the_base() {
  ConfigDoc doc;
  parse(SAMPLE, strlen(SAMPLE), &doc);
  TEST_ASSERT_EQUAL_UINT32(rs::ADDR_NONE, doc.remotes[0].address);
  TEST_ASSERT_EQUAL_UINT32(0x000000, doc.addressOf(0));
  TEST_ASSERT_EQUAL_UINT32(0x000105, doc.addressOf(1));
}

void rejects_a_schema_version_it_does_not_know() {
  const char json[] = "{\"v\":2,\"remotes\":[]}";
  ConfigDoc doc;
  TEST_ASSERT_FALSE(parse(json, strlen(json), &doc));
}

void rejects_malformed_json() {
  const char json[] = "{\"v\":1,\"remotes\":";
  ConfigDoc doc;
  TEST_ASSERT_FALSE(parse(json, strlen(json), &doc));
}

// Truncating would silently drop shutters. Refusing surfaces the problem.
void rejects_an_index_beyond_the_counter_slots() {
  const char json[] = "{\"v\":1,\"remotes\":[{\"i\":30}]}";
  ConfigDoc doc;
  TEST_ASSERT_FALSE(parse(json, strlen(json), &doc));
}

// A removed remote keeps its index reserved, so the document is sparse and the count is
// the highest index plus one. Renumbering survivors would re-key every HA entity.
void remote_count_is_highest_index_plus_one_not_the_entry_count() {
  const char json[] = "{\"v\":1,\"remotes\":[{\"i\":0},{\"i\":5}]}";
  ConfigDoc doc;
  TEST_ASSERT_TRUE(parse(json, strlen(json), &doc));
  TEST_ASSERT_EQUAL_UINT8(2, doc.entries);
  TEST_ASSERT_EQUAL_UINT8(6, doc.remoteCount());
}

void round_trips_through_serialise() {
  ConfigDoc doc;
  parse(SAMPLE, strlen(SAMPLE), &doc);
  char out[1024];
  const size_t n = serialise(doc, out, sizeof(out));
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(out));

  ConfigDoc again;
  TEST_ASSERT_TRUE(parse(out, n, &again));
  TEST_ASSERT_EQUAL_UINT32(doc.epoch, again.epoch);
  TEST_ASSERT_EQUAL_UINT32(doc.base, again.base);
  TEST_ASSERT_EQUAL_UINT8(doc.entries, again.entries);
  TEST_ASSERT_EQUAL_UINT32(doc.addressOf(1), again.addressOf(1));
  TEST_ASSERT_EQUAL_UINT32(contentHash(doc), contentHash(again));
}

void content_hash_ignores_the_epoch_but_notices_a_flag() {
  ConfigDoc a;
  parse(SAMPLE, strlen(SAMPLE), &a);
  ConfigDoc b = a;
  b.epoch = 99;
  TEST_ASSERT_EQUAL_UINT32(contentHash(a), contentHash(b));
  b.remotes[0].enabled = false;
  TEST_ASSERT_NOT_EQUAL(contentHash(a), contentHash(b));
}

void parses_addresses_with_and_without_the_prefix() {
  TEST_ASSERT_EQUAL_UINT32(0xABCDEF, parseHex("0xABCDEF", 0));
  TEST_ASSERT_EQUAL_UINT32(0xABCDEF, parseHex("ABCDEF", 0));
  TEST_ASSERT_EQUAL_UINT32(0, parseHex("0x", 0));
  TEST_ASSERT_EQUAL_UINT32(7, parseHex("nonsense", 7));
  TEST_ASSERT_EQUAL_UINT32(7, parseHex(nullptr, 7));
  TEST_ASSERT_EQUAL_UINT32(7, parseHex("0x1234567890", 7));   // too wide
}

// Adopting on equality makes a lost update silent: two writers reach epoch 8 with
// different content and the device quietly takes whichever landed last.
void equal_epochs_verify_rather_than_adopt() {
  TEST_ASSERT_EQUAL(CFG_VERIFY_ONLY, decide(true, 8, true, 8));
  TEST_ASSERT_EQUAL(CFG_ADOPT, decide(true, 8, true, 9));
  TEST_ASSERT_EQUAL(CFG_REPUBLISH, decide(true, 9, true, 8));
  TEST_ASSERT_EQUAL(CFG_ADOPT, decide(false, 0, true, 1));
  TEST_ASSERT_EQUAL(CFG_REPUBLISH, decide(true, 1, false, 0));
  TEST_ASSERT_EQUAL(CFG_NONE, decide(false, 0, false, 0));
}

void projects_onto_the_store() {
  ConfigDoc doc;
  parse(SAMPLE, strlen(SAMPLE), &doc);
  rs::LiveMap map;
  project(doc, &map);
  TEST_ASSERT_EQUAL_UINT32(0, map.valueOr(rs::NS_SCALAR, rs::SCALAR_ADDRESS_BASE, 0xFF));
  TEST_ASSERT_EQUAL_UINT32(2, map.valueOr(rs::NS_SCALAR, rs::SCALAR_REMOTE_COUNT, 0));
  TEST_ASSERT_EQUAL_UINT32(7, map.valueOr(rs::NS_SCALAR, rs::SCALAR_CONFIG_EPOCH, 0));
  TEST_ASSERT_EQUAL_UINT32(rs::FLAG_ENABLED | rs::FLAG_OPERATIONAL,
                           map.valueOr(rs::NS_FLAGS, 0, 0));
  TEST_ASSERT_EQUAL_UINT32(rs::FLAG_ENABLED, map.valueOr(rs::NS_FLAGS, 1, 0));
}

// Without the explicit clear, a removed remote's override survives compaction forever and
// is silently inherited by whatever later takes that index.
void an_index_the_document_omits_is_cleared_not_left_alone() {
  rs::LiveMap map;
  map.put(rs::NS_ADDR, 4, 0x123456);
  map.put(rs::NS_FLAGS, 4, rs::FLAG_ENABLED);

  ConfigDoc doc;
  parse(SAMPLE, strlen(SAMPLE), &doc);
  project(doc, &map);

  TEST_ASSERT_EQUAL_UINT32(rs::ADDR_NONE, map.valueOr(rs::NS_ADDR, 4, 0));
  TEST_ASSERT_EQUAL_UINT32(0, map.valueOr(rs::NS_FLAGS, 4, 0xFF));
}

void reads_back_out_of_the_store() {
  ConfigDoc doc;
  parse(SAMPLE, strlen(SAMPLE), &doc);
  rs::LiveMap map;
  project(doc, &map);

  ConfigDoc back;
  fromStore(map, &back);
  TEST_ASSERT_EQUAL_UINT32(doc.epoch, back.epoch);
  TEST_ASSERT_EQUAL_UINT8(doc.remoteCount(), back.remoteCount());
  TEST_ASSERT_EQUAL_UINT32(doc.addressOf(1), back.addressOf(1));
  TEST_ASSERT_FALSE(back.remotes[1].operational);
}

// The worst case the MQTT buffer has to survive is not today's twelve.
void worst_case_document_at_thirty_remotes_is_measured_not_assumed() {
  ConfigDoc doc;
  doc.base = 0xFFFFFF;
  strncpy(doc.writer, "device", WRITER_LEN - 1);
  for (uint8_t i = 0; i < rs::MAX_REMOTES; i++) {
    doc.remotes[doc.entries++] = {i, 0xFFFFFF, true, true};
  }
  char out[4096];
  const size_t n = serialise(doc, out, sizeof(out));
  TEST_ASSERT_TRUE(n > 0);
  // Pinned so a format change that outgrows the configured MQTT buffer fails here first.
  TEST_ASSERT_TRUE(n < 2048);

  ConfigDoc again;
  TEST_ASSERT_TRUE(parse(out, n, &again));
  TEST_ASSERT_EQUAL_UINT8(rs::MAX_REMOTES, again.remoteCount());
}

void serialise_reports_truncation_rather_than_emitting_half_a_document() {
  ConfigDoc doc;
  for (uint8_t i = 0; i < rs::MAX_REMOTES; i++) {
    doc.remotes[doc.entries++] = {i, rs::ADDR_NONE, true, true};
  }
  char tiny[64];
  const size_t n = serialise(doc, tiny, sizeof(tiny));
  TEST_ASSERT_EQUAL_size_t(0, n);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(parses_a_document);
  RUN_TEST(a_remote_without_an_address_derives_it_from_the_base);
  RUN_TEST(rejects_a_schema_version_it_does_not_know);
  RUN_TEST(rejects_malformed_json);
  RUN_TEST(rejects_an_index_beyond_the_counter_slots);
  RUN_TEST(remote_count_is_highest_index_plus_one_not_the_entry_count);
  RUN_TEST(round_trips_through_serialise);
  RUN_TEST(content_hash_ignores_the_epoch_but_notices_a_flag);
  RUN_TEST(parses_addresses_with_and_without_the_prefix);
  RUN_TEST(equal_epochs_verify_rather_than_adopt);
  RUN_TEST(projects_onto_the_store);
  RUN_TEST(an_index_the_document_omits_is_cleared_not_left_alone);
  RUN_TEST(reads_back_out_of_the_store);
  RUN_TEST(worst_case_document_at_thirty_remotes_is_measured_not_assumed);
  RUN_TEST(serialise_reports_truncation_rather_than_emitting_half_a_document);
  return UNITY_END();
}
