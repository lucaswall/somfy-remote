#include <stdio.h>
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

// --- travel time --------------------------------------------------------------------------

// It rides in the flags record rather than a namespace of its own, so the round trip through
// the store is the thing worth pinning: a shift collision would silently reset every shutter
// to the default travel time and nothing else would notice.
static void travel_time_survives_the_store(void) {
  cfg::ConfigDoc doc;
  doc.base = 0x000000;
  doc.entries = 2;
  doc.remotes[0] = {0, rs::ADDR_NONE, true, true, 22};
  doc.remotes[1] = {1, rs::ADDR_NONE, true, false, 255};

  rs::LiveMap map;
  cfg::project(doc, &map);
  map.put(rs::NS_SCALAR, rs::SCALAR_REMOTE_COUNT, 2);

  cfg::ConfigDoc back;
  cfg::fromStore(map, &back);
  TEST_ASSERT_EQUAL_UINT8(22, back.remotes[0].travelSeconds);
  TEST_ASSERT_EQUAL_UINT8(255, back.remotes[1].travelSeconds);
  TEST_ASSERT_TRUE(back.remotes[0].enabled);
  TEST_ASSERT_TRUE(back.remotes[0].operational);
  TEST_ASSERT_FALSE(back.remotes[1].operational);
}

static void travel_time_round_trips_through_json(void) {
  cfg::ConfigDoc doc;
  doc.entries = 1;
  doc.remotes[0] = {3, rs::ADDR_NONE, true, true, 19};
  char json[512];
  const size_t n = cfg::serialise(doc, json, sizeof(json));
  TEST_ASSERT_GREATER_THAN_size_t(0, n);

  cfg::ConfigDoc back;
  TEST_ASSERT_TRUE(cfg::parse(json, n, &back));
  TEST_ASSERT_EQUAL_UINT8(19, back.remotes[0].travelSeconds);
}

// A document written before travel times existed must still parse, and must not claim its
// shutters travel in no time at all.
static void a_document_without_a_travel_time_uses_the_default(void) {
  cfg::ConfigDoc doc;
  const char json[] =
      "{\"v\":1,\"epoch\":1,\"base\":\"0x000000\","
      "\"remotes\":[{\"i\":0,\"enabled\":true,\"operational\":true}]}";
  TEST_ASSERT_TRUE(cfg::parse(json, strlen(json), &doc));
  TEST_ASSERT_EQUAL_UINT8(0, doc.remotes[0].travelSeconds);
}

// Changing it is a configuration change like any other, or two boards would disagree about
// the document while both believing they held the same one.
static void travel_time_changes_the_content_hash(void) {
  cfg::ConfigDoc a;
  a.entries = 1;
  a.remotes[0] = {0, rs::ADDR_NONE, true, true, 22};
  cfg::ConfigDoc b = a;
  b.remotes[0].travelSeconds = 30;
  TEST_ASSERT_NOT_EQUAL(cfg::contentHash(a), cfg::contentHash(b));
}

// A flag that exists to stop a motor must never be granted by a type mismatch. These are
// the shapes a template renderer produces when a JSON serialiser would have produced a
// boolean.
static bool parsesFlagDoc(const char *flagJson) {
  char json[192];
  snprintf(json, sizeof(json),
           "{\"v\":1,\"epoch\":7,\"base\":\"0x000000\",\"remotes\":[{\"i\":9,%s}]}",
           flagJson);
  ConfigDoc doc;
  return parse(json, strlen(json), &doc);
}

void a_quoted_boolean_flag_refuses_the_document() {
  TEST_ASSERT_FALSE(parsesFlagDoc("\"operational\":\"false\""));
  TEST_ASSERT_FALSE(parsesFlagDoc("\"enabled\":\"true\""));
}

void a_numeric_flag_refuses_the_document() {
  TEST_ASSERT_FALSE(parsesFlagDoc("\"operational\":0"));
  TEST_ASSERT_FALSE(parsesFlagDoc("\"enabled\":1"));
}

void a_null_flag_takes_the_default_rather_than_refusing() {
  TEST_ASSERT_TRUE(parsesFlagDoc("\"operational\":null"));
}

void an_absent_flag_still_defaults_to_true() {
  char json[128];
  snprintf(json, sizeof(json),
           "{\"v\":1,\"epoch\":7,\"base\":\"0x000000\",\"remotes\":[{\"i\":9}]}");
  ConfigDoc doc;
  TEST_ASSERT_TRUE(parse(json, strlen(json), &doc));
  TEST_ASSERT_TRUE(doc.remotes[0].operational);
  TEST_ASSERT_TRUE(doc.remotes[0].enabled);
}

void address_parsing_accepts_what_an_rts_address_looks_like() {
  uint32_t v = 0;
  TEST_ASSERT_TRUE(parseAddress("0x0A1B2C", &v));
  TEST_ASSERT_EQUAL_UINT32(0x0A1B2Cu, v);
  TEST_ASSERT_TRUE(parseAddress("0a1b2c", &v));
  TEST_ASSERT_EQUAL_UINT32(0x0A1B2Cu, v);
  TEST_ASSERT_TRUE(parseAddress("1", &v));
  TEST_ASSERT_EQUAL_UINT32(1u, v);
}

void address_parsing_refuses_what_would_be_silently_truncated() {
  uint32_t v = 0;
  TEST_ASSERT_FALSE(parseAddress("1a2b3g", &v));    // not hex
  TEST_ASSERT_FALSE(parseAddress("1a2b3c4d", &v));  // wider than 24 bits
  TEST_ASSERT_FALSE(parseAddress("ffffffff", &v));  // would land on ADDR_NONE
  TEST_ASSERT_FALSE(parseAddress("0x", &v));
  TEST_ASSERT_FALSE(parseAddress("", &v));
  TEST_ASSERT_FALSE(parseAddress(nullptr, &v));
}

void a_document_carrying_an_oversized_address_is_refused() {
  static const char json[] =
      "{\"v\":1,\"epoch\":7,\"base\":\"0x000000\",\"remotes\":["
      "{\"i\":0,\"addr\":\"0x1a2b3c4d\"}]}";
  ConfigDoc doc;
  TEST_ASSERT_FALSE(parse(json, strlen(json), &doc));
}

static ConfigDoc docOf(const char *json) {
  ConfigDoc d;
  TEST_ASSERT_TRUE(parse(json, strlen(json), &d));
  return d;
}

void a_remote_that_loses_enabled_gives_up_its_entities() {
  const ConfigDoc from = docOf(
      "{\"v\":1,\"epoch\":1,\"base\":\"0x0\",\"remotes\":["
      "{\"i\":0,\"enabled\":true},{\"i\":1,\"enabled\":true}]}");
  const ConfigDoc to = docOf(
      "{\"v\":1,\"epoch\":2,\"base\":\"0x0\",\"remotes\":["
      "{\"i\":0,\"enabled\":true},{\"i\":1,\"enabled\":false}]}");
  TEST_ASSERT_EQUAL_UINT32(1u << 1, entitiesToRemove(from, to));
}

void a_remote_that_falls_off_a_shrunken_count_gives_up_its_entities() {
  const ConfigDoc from = docOf(
      "{\"v\":1,\"epoch\":1,\"base\":\"0x0\",\"remotes\":["
      "{\"i\":0,\"enabled\":true},{\"i\":2,\"enabled\":true}]}");
  const ConfigDoc to = docOf(
      "{\"v\":1,\"epoch\":2,\"base\":\"0x0\",\"remotes\":[{\"i\":0,\"enabled\":true}]}");
  TEST_ASSERT_EQUAL_UINT32(1u << 2, entitiesToRemove(from, to));
}

void a_still_enabled_or_newly_added_remote_keeps_its_entities() {
  const ConfigDoc from = docOf(
      "{\"v\":1,\"epoch\":1,\"base\":\"0x0\",\"remotes\":[{\"i\":0,\"enabled\":true}]}");
  const ConfigDoc to = docOf(
      "{\"v\":1,\"epoch\":2,\"base\":\"0x0\",\"remotes\":["
      "{\"i\":0,\"enabled\":true},{\"i\":1,\"enabled\":true}]}");
  TEST_ASSERT_EQUAL_UINT32(0u, entitiesToRemove(from, to));
}

// The gallery roof is held off the air by operational, and keeps its cards on purpose.
void a_remote_flagged_not_operational_keeps_its_entities() {
  const ConfigDoc from = docOf(
      "{\"v\":1,\"epoch\":1,\"base\":\"0x0\",\"remotes\":["
      "{\"i\":0,\"enabled\":true,\"operational\":true}]}");
  const ConfigDoc to = docOf(
      "{\"v\":1,\"epoch\":2,\"base\":\"0x0\",\"remotes\":["
      "{\"i\":0,\"enabled\":true,\"operational\":false}]}");
  TEST_ASSERT_EQUAL_UINT32(0u, entitiesToRemove(from, to));
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
  RUN_TEST(travel_time_survives_the_store);
  RUN_TEST(travel_time_round_trips_through_json);
  RUN_TEST(a_document_without_a_travel_time_uses_the_default);
  RUN_TEST(a_quoted_boolean_flag_refuses_the_document);
  RUN_TEST(a_numeric_flag_refuses_the_document);
  RUN_TEST(a_null_flag_takes_the_default_rather_than_refusing);
  RUN_TEST(an_absent_flag_still_defaults_to_true);
  RUN_TEST(address_parsing_accepts_what_an_rts_address_looks_like);
  RUN_TEST(address_parsing_refuses_what_would_be_silently_truncated);
  RUN_TEST(a_document_carrying_an_oversized_address_is_refused);
  RUN_TEST(a_remote_that_loses_enabled_gives_up_its_entities);
  RUN_TEST(a_remote_that_falls_off_a_shrunken_count_gives_up_its_entities);
  RUN_TEST(a_still_enabled_or_newly_added_remote_keeps_its_entities);
  RUN_TEST(a_remote_flagged_not_operational_keeps_its_entities);
  RUN_TEST(travel_time_changes_the_content_hash);
  return UNITY_END();
}
