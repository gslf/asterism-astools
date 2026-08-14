/*
 * test_util.c — util.c: buffers, base64, slugs, strings (§17).
 */

#include "astools_test.h"

#include "astools_internal.h"

/* ---- base64 -------------------------------------------------------------- */

static void round_trip(const uint8_t *data, size_t len, const char *want) {
  char *enc = astools_base64_encode(data, len);
  uint8_t *dec = NULL;
  size_t dec_len = 0;
  ASSERT_TRUE(enc != NULL);
  if (want != NULL) ASSERT_EQ_STR(enc, want);
  ASSERT_TRUE(astools_base64_decode(enc, &dec, &dec_len));
  ASSERT_EQ_INT(dec_len, len);
  ASSERT_TRUE(len == 0 || memcmp(dec, data, len) == 0);
  free(enc);
  free(dec);
}

TEST(base64_round_trips) {
  uint8_t all[256];
  size_t i;
  for (i = 0; i < sizeof all; i++) all[i] = (uint8_t)i;
  round_trip((const uint8_t *)"", 0, "");
  round_trip((const uint8_t *)"f", 1, "Zg==");
  round_trip((const uint8_t *)"fo", 2, "Zm8=");
  round_trip((const uint8_t *)"foo", 3, "Zm9v");
  round_trip((const uint8_t *)"foob", 4, "Zm9vYg==");
  round_trip((const uint8_t *)"fooba", 5, "Zm9vYmE=");
  round_trip((const uint8_t *)"foobar", 6, "Zm9vYmFy");
  round_trip(all, sizeof all, NULL);
}

TEST(base64_invalid_decode) {
  uint8_t *out = NULL;
  size_t n = 0;
  /* wrong length (not a multiple of 4) */
  ASSERT_TRUE(!astools_base64_decode("A", &out, &n));
  ASSERT_TRUE(!astools_base64_decode("AAA", &out, &n));
  /* bad alphabet */
  ASSERT_TRUE(!astools_base64_decode("Zm9%", &out, &n));
  ASSERT_TRUE(!astools_base64_decode("Zm9v\n", &out, &n));
  /* interior '=' */
  ASSERT_TRUE(!astools_base64_decode("Zm=v", &out, &n));
  ASSERT_TRUE(!astools_base64_decode("=AAA", &out, &n));
  /* non-canonical trailing bits: 'B' = 000001, low bits set under pad 2 */
  ASSERT_TRUE(!astools_base64_decode("AB==", &out, &n));
  ASSERT_TRUE(!astools_base64_decode(NULL, &out, &n));
}

TEST(base64_decode_valid_padding) {
  uint8_t *out = NULL;
  size_t n = 0;
  ASSERT_TRUE(astools_base64_decode("AQ==", &out, &n));
  ASSERT_EQ_INT(n, 1);
  ASSERT_EQ_INT(out[0], 0x01);
  free(out);
  out = NULL;
  ASSERT_TRUE(astools_base64_decode("", &out, &n));
  ASSERT_EQ_INT(n, 0);
  free(out);
}

/* ---- slug ---------------------------------------------------------------- */

TEST(slug_rules) {
  char long_ok[65], long_bad[66];
  memset(long_ok, 'a', 64);
  long_ok[64] = '\0';
  memset(long_bad, 'a', 65);
  long_bad[65] = '\0';

  ASSERT_TRUE(astools_slug_valid("a"));
  ASSERT_TRUE(astools_slug_valid("0"));
  ASSERT_TRUE(astools_slug_valid("fs"));
  ASSERT_TRUE(astools_slug_valid("my-tool-2"));
  ASSERT_TRUE(astools_slug_valid("a0-b"));
  ASSERT_TRUE(astools_slug_valid(long_ok)); /* 64 chars: max */

  ASSERT_TRUE(!astools_slug_valid(NULL));
  ASSERT_TRUE(!astools_slug_valid(""));
  ASSERT_TRUE(!astools_slug_valid("-a"));   /* leading dash */
  ASSERT_TRUE(!astools_slug_valid("A"));    /* uppercase */
  ASSERT_TRUE(!astools_slug_valid("aB"));
  ASSERT_TRUE(!astools_slug_valid("a_b"));  /* underscore (D7) */
  ASSERT_TRUE(!astools_slug_valid("a.b"));
  ASSERT_TRUE(!astools_slug_valid("a b"));
  ASSERT_TRUE(!astools_slug_valid(long_bad)); /* 65 chars */
}

/* ---- buffer -------------------------------------------------------------- */

TEST(buf_growth_and_termination) {
  astools_buf b;
  size_t i;
  astools_buf_init(&b);
  for (i = 0; i < 10000; i++) ASSERT_OK(astools_buf_appendc(&b, 'x'));
  ASSERT_EQ_INT(b.len, 10000);
  ASSERT_TRUE(b.data != NULL);
  ASSERT_EQ_INT(b.data[10000], '\0');
  for (i = 0; i < 10000; i++) {
    if (b.data[i] != 'x') {
      ASTOOLS_FAILF("byte %zu is not 'x'", i);
      astools_buf_free(&b);
      return;
    }
  }
  ASSERT_OK(astools_buf_appends(&b, "tail"));
  ASSERT_EQ_INT(b.len, 10004);
  ASSERT_EQ_STR(b.data + 10000, "tail");
  astools_buf_free(&b);
  ASSERT_TRUE(b.data == NULL);
  ASSERT_EQ_INT(b.len, 0);
}

TEST(buf_printf_and_detach) {
  astools_buf b;
  char *s;
  astools_buf_init(&b);
  ASSERT_OK(astools_buf_printf(&b, "%s=%d", "answer", 42));
  ASSERT_OK(astools_buf_printf(&b, " %04x", 0xbeefu));
  ASSERT_EQ_STR(b.data, "answer=42 beef");
  s = astools_buf_detach(&b);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ_STR(s, "answer=42 beef");
  ASSERT_TRUE(b.data == NULL);
  ASSERT_EQ_INT(b.len, 0);
  free(s);
  /* detach of an empty buffer yields an empty string, not NULL */
  s = astools_buf_detach(&b);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ_STR(s, "");
  free(s);
}

TEST(buf_append_binary) {
  astools_buf b;
  astools_buf_init(&b);
  ASSERT_OK(astools_buf_append(&b, "a\0b", 3));
  ASSERT_EQ_INT(b.len, 3);
  ASSERT_EQ_INT(b.data[1], '\0');
  ASSERT_EQ_INT(b.data[2], 'b');
  ASSERT_EQ_INT(b.data[3], '\0');
  astools_buf_free(&b);
}

/* ---- strings ------------------------------------------------------------- */

TEST(string_helpers) {
  char trim1[] = "  hello \t\n";
  char trim2[] = "\r\n";
  char *dup;

  ASSERT_EQ_STR(astools_str_trim(trim1), "hello");
  ASSERT_EQ_STR(astools_str_trim(trim2), "");
  ASSERT_TRUE(astools_str_blank(NULL));
  ASSERT_TRUE(astools_str_blank(""));
  ASSERT_TRUE(astools_str_blank(" \t\r\n"));
  ASSERT_TRUE(!astools_str_blank(" x "));

  ASSERT_TRUE(astools_strdup(NULL) == NULL);
  dup = astools_strdup("abc");
  ASSERT_EQ_STR(dup, "abc");
  free(dup);
  dup = astools_strndup("abcdef", 3);
  ASSERT_EQ_STR(dup, "abc");
  free(dup);
  dup = astools_strndup("ab", 10); /* stops at NUL */
  ASSERT_EQ_STR(dup, "ab");
  free(dup);
}

TEST(fnv1a64_known_vectors) {
  /* Published FNV-1a 64 test vectors. */
  ASSERT_TRUE(astools_fnv1a64("", 0) == UINT64_C(0xcbf29ce484222325));
  ASSERT_TRUE(astools_fnv1a64("a", 1) == UINT64_C(0xaf63dc4c8601ec8c));
  ASSERT_TRUE(astools_fnv1a64("foobar", 6) == UINT64_C(0x85944171f73967e8));
}

TEST_LIST = {
  TEST_ENTRY(base64_round_trips),
  TEST_ENTRY(base64_invalid_decode),
  TEST_ENTRY(base64_decode_valid_padding),
  TEST_ENTRY(slug_rules),
  TEST_ENTRY(buf_growth_and_termination),
  TEST_ENTRY(buf_printf_and_detach),
  TEST_ENTRY(buf_append_binary),
  TEST_ENTRY(string_helpers),
  TEST_ENTRY(fnv1a64_known_vectors),
};

RUN_ALL_TESTS()
