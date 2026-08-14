/*
 * test_time.c — time.c: RFC 3339 parse/format, ISO 8601 durations (§17).
 */

#include "astools_test.h"

#include "astools_internal.h"

/* ---- RFC 3339 ------------------------------------------------------------ */

static void rt(const char *in, const char *want_utc) {
  astools_time t = 0;
  char out[32];
  if (!astools_time_parse_rfc3339(in, &t)) {
    ASTOOLS_FAILF("parse rejected '%s'", in);
    return;
  }
  astools_time_format_rfc3339(t, out);
  ASSERT_EQ_STR(out, want_utc);
}

TEST(rfc3339_round_trips) {
  rt("1970-01-01T00:00:00Z", "1970-01-01T00:00:00Z");
  rt("2026-08-02T10:15:32Z", "2026-08-02T10:15:32Z");
  rt("2026-08-02t10:15:32z", "2026-08-02T10:15:32Z"); /* lowercase t/z */
  rt("2000-02-29T23:59:59Z", "2000-02-29T23:59:59Z"); /* leap day */
  rt("1969-12-31T23:59:59Z", "1969-12-31T23:59:59Z"); /* pre-epoch */
  /* fractional seconds are accepted and truncated */
  rt("2026-08-02T10:15:32.75Z", "2026-08-02T10:15:32Z");
}

TEST(rfc3339_offsets) {
  astools_time a = 0, b = 0, c = 0;
  ASSERT_TRUE(astools_time_parse_rfc3339("2026-08-02T10:15:32Z", &a));
  ASSERT_TRUE(astools_time_parse_rfc3339("2026-08-02T12:15:32+02:00", &b));
  ASSERT_TRUE(astools_time_parse_rfc3339("2026-08-02T05:45:32-04:30", &c));
  ASSERT_EQ_INT(b, a);
  ASSERT_EQ_INT(c, a);
  rt("2026-01-01T00:30:00+01:00", "2025-12-31T23:30:00Z"); /* day rollover */
}

TEST(rfc3339_known_epoch) {
  astools_time t = 0;
  ASSERT_TRUE(astools_time_parse_rfc3339("2001-09-09T01:46:40Z", &t));
  ASSERT_EQ_INT(t, 1000000000);
}

TEST(rfc3339_rejects) {
  astools_time t = 0;
  ASSERT_TRUE(!astools_time_parse_rfc3339(NULL, &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02 10:15:32Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-13-01T00:00:00Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-00-01T00:00:00Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-02-30T00:00:00Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2025-02-29T00:00:00Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T24:00:00Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:60:00Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:61Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:32", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:32Zx", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:32+2:00", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:32+02:60", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:32+24:00", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("2026-08-02T10:15:32.Z", &t));
  ASSERT_TRUE(!astools_time_parse_rfc3339("x026-08-02T10:15:32Z", &t));
}

/* ---- durations ----------------------------------------------------------- */

static void dur_ok(const char *in, int64_t want) {
  int64_t got = -1;
  if (!astools_duration_parse(in, &got)) {
    ASTOOLS_FAILF("duration rejected '%s'", in);
    return;
  }
  ASSERT_EQ_INT(got, want);
}

TEST(duration_accepts) {
  dur_ok("PT30S", 30);
  dur_ok("PT2M", 120);      /* PT1M is MINUTES */
  dur_ok("PT1M", 60);
  dur_ok("PT1H30M", 5400);
  dur_ok("P1DT1S", 86401);
  dur_ok("PT0S", 0);
  dur_ok("P1D", 86400);
  dur_ok("P1W", 604800);
  dur_ok("PT2H3M4S", 7384);
}

TEST(duration_rejects) {
  int64_t out = 0;
  /* P1M is MONTHS => rejected (calendar-dependent, §time.c contract) */
  ASSERT_TRUE(!astools_duration_parse("P1M", &out));
  ASSERT_TRUE(!astools_duration_parse("P1Y", &out));
  ASSERT_TRUE(!astools_duration_parse("P1Y2M3D", &out));
  ASSERT_TRUE(!astools_duration_parse(NULL, &out));
  ASSERT_TRUE(!astools_duration_parse("", &out));
  ASSERT_TRUE(!astools_duration_parse("P", &out));
  ASSERT_TRUE(!astools_duration_parse("PT", &out));
  ASSERT_TRUE(!astools_duration_parse("30S", &out));
  ASSERT_TRUE(!astools_duration_parse("PT30", &out));   /* missing unit */
  ASSERT_TRUE(!astools_duration_parse("PT1.5S", &out)); /* fractional */
  ASSERT_TRUE(!astools_duration_parse("PT1S2H", &out)); /* wrong order */
  ASSERT_TRUE(!astools_duration_parse("PT1H1H", &out)); /* repeated unit */
  ASSERT_TRUE(!astools_duration_parse("P1W1D", &out));  /* W is exclusive */
  ASSERT_TRUE(!astools_duration_parse("PT-1S", &out));
  ASSERT_TRUE(!astools_duration_parse("PT1S ", &out));
}

/* ---- injectable clock ---------------------------------------------------- */

static astools_time fixed_now(void *ud) { return *(astools_time *)ud; }

TEST(clock_injectable) {
  astools_time fixed = 12345;
  astools_clock clk;
  clk.ud = &fixed;
  clk.now = fixed_now;
  ASSERT_EQ_INT(astools_clock_now(&clk), 12345);
  fixed = 777;
  ASSERT_EQ_INT(astools_clock_now(&clk), 777);
  /* NULL clock falls back to the system clock (sanity: it advances) */
  ASSERT_TRUE(astools_clock_now(NULL) > 1500000000);
}

TEST_LIST = {
  TEST_ENTRY(rfc3339_round_trips),
  TEST_ENTRY(rfc3339_offsets),
  TEST_ENTRY(rfc3339_known_epoch),
  TEST_ENTRY(rfc3339_rejects),
  TEST_ENTRY(duration_accepts),
  TEST_ENTRY(duration_rejects),
  TEST_ENTRY(clock_injectable),
};

RUN_ALL_TESTS()
