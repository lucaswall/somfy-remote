#include <unity.h>

#include "http_origin.h"

using namespace http;

void setUp() {}
void tearDown() {}

static const char *HOSTNAME = "somfy-remote";
static const char *IP = "192.0.2.10";

void the_three_names_this_device_answers_to_are_accepted() {
  TEST_ASSERT_TRUE(hostIsOurs("somfy-remote", HOSTNAME, IP));
  TEST_ASSERT_TRUE(hostIsOurs("somfy-remote.local", HOSTNAME, IP));
  TEST_ASSERT_TRUE(hostIsOurs("192.0.2.10", HOSTNAME, IP));
}

void a_port_is_not_part_of_the_name() {
  TEST_ASSERT_TRUE(hostIsOurs("192.0.2.10:80", HOSTNAME, IP));
  TEST_ASSERT_TRUE(hostIsOurs("somfy-remote.local:80", HOSTNAME, IP));
}

// Hostnames are case-insensitive, and a browser is free to send any casing.
void casing_does_not_matter() {
  TEST_ASSERT_TRUE(hostIsOurs("Somfy-Remote.LOCAL", HOSTNAME, IP));
  TEST_ASSERT_TRUE(hostIsOurs("SOMFY-REMOTE", HOSTNAME, IP));
}

// The rebinding case: the attacker's name resolves to this device's address.
void a_name_this_device_does_not_answer_to_is_refused() {
  TEST_ASSERT_FALSE(hostIsOurs("attacker.example", HOSTNAME, IP));
  TEST_ASSERT_FALSE(hostIsOurs("somfy-remote.attacker.example", HOSTNAME, IP));
  TEST_ASSERT_FALSE(hostIsOurs("somfy-remote.lan", HOSTNAME, IP));
  TEST_ASSERT_FALSE(hostIsOurs("", HOSTNAME, IP));
  TEST_ASSERT_FALSE(hostIsOurs(nullptr, HOSTNAME, IP));
}

void an_origin_authority_must_be_the_host() {
  TEST_ASSERT_TRUE(originMatchesHost("http://somfy-remote.local", "somfy-remote.local"));
  TEST_ASSERT_TRUE(originMatchesHost("https://192.0.2.10:80", "192.0.2.10:80"));
  TEST_ASSERT_FALSE(originMatchesHost("http://attacker.example", "somfy-remote.local"));
}

// No "//" at all: treated as a bare authority rather than silently accepted.
void an_origin_without_a_scheme_is_compared_whole() {
  TEST_ASSERT_TRUE(originMatchesHost("somfy-remote.local", "somfy-remote.local"));
  TEST_ASSERT_FALSE(originMatchesHost("attacker.example", "somfy-remote.local"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(the_three_names_this_device_answers_to_are_accepted);
  RUN_TEST(a_port_is_not_part_of_the_name);
  RUN_TEST(casing_does_not_matter);
  RUN_TEST(a_name_this_device_does_not_answer_to_is_refused);
  RUN_TEST(an_origin_authority_must_be_the_host);
  RUN_TEST(an_origin_without_a_scheme_is_compared_whole);
  return UNITY_END();
}
