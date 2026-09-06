/*
 * test_time.c — time.c: RFC 3339 parse/format, ISO 8601 durations.
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
  if (!astools_duration_parse_ms(in, &got)) {
    ASTOOLS_FAILF("duration rejected '%s'", in);
    return;
  }
  ASSERT_EQ_INT(got, want * 1000);
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
  /* P1M is MONTHS => rejected (calendar-dependent) */
  ASSERT_TRUE(!astools_duration_parse_ms("P1M", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("P1Y", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("P1Y2M3D", &out));
  ASSERT_TRUE(!astools_duration_parse_ms(NULL, &out));
  ASSERT_TRUE(!astools_duration_parse_ms("", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("P", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("PT", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("30S", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("PT30", &out));   /* missing unit */
  ASSERT_TRUE(!astools_duration_parse_ms("PT0.0001S", &out)); /* below precision */
  ASSERT_TRUE(!astools_duration_parse_ms("PT1S2H", &out)); /* wrong order */
  ASSERT_TRUE(!astools_duration_parse_ms("PT1H1H", &out)); /* repeated unit */
  ASSERT_TRUE(!astools_duration_parse_ms("P1W1D", &out));  /* W is exclusive */
  ASSERT_TRUE(!astools_duration_parse_ms("PT-1S", &out));
  ASSERT_TRUE(!astools_duration_parse_ms("PT1S ", &out));
}

TEST(duration_milliseconds_are_exact_and_bounded) {
  int64_t out = 0;
  const struct { const char *text; int64_t value; } cases[] = {
    {"PT0.001S",1}, {"PT1.01S",1010}, {"P1DT2H3M4.005S",93784005},
    {"PT9223372036854775.807S",INT64_MAX}
  };
  for (size_t i = 0; i < sizeof cases/sizeof *cases; i++) {
    ASSERT_TRUE(astools_duration_parse_ms(cases[i].text,&out));
    ASSERT_EQ_INT(out,cases[i].value);
  }
  const char *bad[] = {"PT9223372036854775.808S","P9999999999999999999D",
    "PT1.5H","PT1e2S","PT0,1S","PT1.S","PT.1S","PT0.0001S","P1DT","PT1.2S1S"};
  for (size_t i = 0; i < sizeof bad/sizeof *bad; i++) {
    out = 23; ASSERT_TRUE(!astools_duration_parse_ms(bad[i],&out)); ASSERT_EQ_INT(out,23);
  }
}

TEST(config_periods_preserve_milliseconds_without_wrapping) {
  char root[256], path[512]; ASSERT_TRUE(astools_test_tmpdir(root));
  snprintf(path,sizeof path,"%s/config.xcdn",root);
  const char *durations[] = {"PT0.125S","PT4294967.294S","PT4294967.295S","PT0.0001S"};
  for (size_t i = 0; i < sizeof durations/sizeof *durations; i++) {
    FILE *file = fopen(path,"wb"); ASSERT_TRUE(file != NULL);
    ASSERT_TRUE(fprintf(file,"#astools_config {invocation:{timeout:r\"%s\"}}",durations[i]) > 0);
    ASSERT_EQ_INT(fclose(file),0);
    astools_config cfg; astools_config_defaults(&cfg); char *error = NULL;
    astools_err e = astools_config_load(path,true,&cfg,&error);
    ASSERT_EQ_INT(e,i < 2 ? ASTOOLS_OK : ASTOOLS_ERR_CONFIG);
    if (i < 2) ASSERT_EQ_INT(cfg.timeout_ms,i ? ASTOOLS_PERIOD_MAX_MS : 125);
    astools_config_free(&cfg); free(error);
  }
  astools_test_rmtree(root);
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
  TEST_ENTRY(duration_milliseconds_are_exact_and_bounded),
  TEST_ENTRY(config_periods_preserve_milliseconds_without_wrapping),
  TEST_ENTRY(clock_injectable),
};

RUN_ALL_TESTS()
