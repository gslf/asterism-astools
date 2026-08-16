/*
 * test_json.c — mcp/json.c: strict RFC 8259 codec (jx). Compiled
 * against mcp/json.c directly.
 */

#include "astools_test.h"

#include "json.h"

/* Round trip source -> parse -> compact write; expect `want` (or the
 * source itself when want == NULL). */
static void rt(const char *src, const char *want) {
  jx_value *v = NULL;
  char *out;
  if (jx_parse(src, strlen(src), &v) != 0) {
    ASTOOLS_FAILF("parse failed for %s", src);
    return;
  }
  out = jx_write(v, 0);
  jx_free(v);
  if (!out) {
    ASTOOLS_FAILF("write failed for %s", src);
    return;
  }
  if (strcmp(out, want != NULL ? want : src) != 0)
    ASTOOLS_FAILF("round trip %s -> %s (want %s)", src, out,
                want != NULL ? want : src);
  free(out);
}

static int rejects(const char *src) {
  jx_value *v = NULL;
  if (jx_parse(src, strlen(src), &v) == 0) {
    jx_free(v);
    return 0;
  }
  return v == NULL;
}

TEST(round_trips) {
  rt("null", NULL);
  rt("true", NULL);
  rt("false", NULL);
  rt("0", NULL);
  rt("-42", NULL);
  rt("2.5", NULL);
  rt("\"\"", NULL);
  rt("\"hi\"", NULL);
  rt("[]", NULL);
  rt("{}", NULL);
  rt("{\"a\":[1,2.5,\"x\",true,null],\"b\":{\"c\":[[]]}}", NULL);
  /* insignificant whitespace is dropped by the compact writer */
  rt(" { \"a\" : [ 1 , 2 ] } ", "{\"a\":[1,2]}");
}

TEST(escapes) {
  rt("\"a\\nb\"", NULL);
  rt("\"tab\\there\"", NULL);
  rt("\"q\\\"q\"", NULL);
  rt("\"back\\\\slash\"", NULL);
  /* \u0041 = 'A': decoded on parse, written raw */
  rt("\"\\u0041\"", "\"A\"");
  /* control characters re-escape */
  rt("\"\\u0001\"", "\"\\u0001\"");
  {
    jx_value *v = NULL;
    ASSERT_EQ_INT(jx_parse("\"a\\u0000b\"", 10, &v), 0);
    ASSERT_EQ_INT(jx_string_length(v), 3); /* embedded NUL preserved */
    jx_free(v);
  }
}

TEST(surrogate_pairs) {
  jx_value *v = NULL;
  const char *s;
  char *out;
  /* U+1F600 as a surrogate pair decodes to 4 UTF-8 bytes */
  ASSERT_EQ_INT(jx_parse("\"\\ud83d\\ude00\"", 14, &v), 0);
  s = jx_string_value(v);
  ASSERT_TRUE(s != NULL);
  ASSERT_EQ_INT(jx_string_length(v), 4);
  ASSERT_EQ_INT((unsigned char)s[0], 0xF0);
  ASSERT_EQ_INT((unsigned char)s[1], 0x9F);
  ASSERT_EQ_INT((unsigned char)s[2], 0x98);
  ASSERT_EQ_INT((unsigned char)s[3], 0x80);
  out = jx_write(v, 0);
  jx_free(v);
  ASSERT_TRUE(out != NULL);
  /* writer emits the raw UTF-8 bytes */
  ASSERT_EQ_STR(out, "\"\xF0\x9F\x98\x80\"");
  free(out);
  /* lone surrogates are invalid in a strict codec */
  ASSERT_TRUE(rejects("\"\\ud83d\""));
  ASSERT_TRUE(rejects("\"\\ude00\""));
  ASSERT_TRUE(rejects("\"\\ud83dx\""));
}

TEST(depth_cap) {
  /* JX_MAX_DEPTH = 64: 64 nested arrays parse, 65 reject */
  char ok[64 + 64 + 1], too_deep[65 + 65 + 1];
  int i;
  for (i = 0; i < 64; i++) {
    ok[i] = '[';
    ok[64 + i] = ']';
  }
  ok[128] = '\0';
  for (i = 0; i < 65; i++) {
    too_deep[i] = '[';
    too_deep[65 + i] = ']';
  }
  too_deep[130] = '\0';
  {
    jx_value *v = NULL;
    ASSERT_EQ_INT(jx_parse(ok, strlen(ok), &v), 0);
    jx_free(v);
  }
  ASSERT_TRUE(rejects(too_deep));
}

TEST(trailing_garbage_rejected) {
  ASSERT_TRUE(rejects("1 x"));
  ASSERT_TRUE(rejects("{} {}"));
  ASSERT_TRUE(rejects("[1,2]]"));
  ASSERT_TRUE(rejects("\"a\"b"));
  ASSERT_TRUE(rejects(""));
  ASSERT_TRUE(rejects("   "));
}

TEST(strictness_rejects) {
  ASSERT_TRUE(rejects("NaN"));
  ASSERT_TRUE(rejects("Infinity"));
  ASSERT_TRUE(rejects("-Infinity"));
  ASSERT_TRUE(rejects("nan"));
  ASSERT_TRUE(rejects("01"));       /* leading zero */
  ASSERT_TRUE(rejects("1."));
  ASSERT_TRUE(rejects(".5"));
  ASSERT_TRUE(rejects("+1"));
  ASSERT_TRUE(rejects("'a'"));      /* single quotes */
  ASSERT_TRUE(rejects("{a:1}"));    /* unquoted key */
  ASSERT_TRUE(rejects("[1,]"));     /* trailing comma */
  ASSERT_TRUE(rejects("{\"a\":1,}"));
  ASSERT_TRUE(rejects("// c\n1"));  /* comments */
  ASSERT_TRUE(rejects("\"a\nb\"")); /* raw control char in string */
  ASSERT_TRUE(rejects("\"\xFF\"")); /* invalid UTF-8 */
  ASSERT_TRUE(rejects("truex"));
  ASSERT_TRUE(rejects("{\"a\"}"));
}

TEST(number_views) {
  jx_value *v = NULL;
  ASSERT_EQ_INT(jx_parse("5", 1, &v), 0);
  ASSERT_TRUE(jx_is_int(v));
  ASSERT_EQ_INT(jx_int_value(v), 5);
  jx_free(v);
  v = NULL;
  ASSERT_EQ_INT(jx_parse("5.5", 3, &v), 0);
  ASSERT_TRUE(!jx_is_int(v));
  ASSERT_EQ_DBL(jx_double_value(v), 5.5, 1e-12);
  jx_free(v);
  v = NULL;
  ASSERT_EQ_INT(jx_parse("-9223372036854775808", 20, &v), 0);
  ASSERT_TRUE(jx_is_int(v));
  jx_free(v);
}

TEST(constructors_and_containers) {
  jx_value *obj = jx_object(), *arr = jx_array();
  char *out;
  ASSERT_TRUE(obj != NULL && arr != NULL);
  ASSERT_EQ_INT(jx_array_push(arr, jx_int(1)), 0);
  ASSERT_EQ_INT(jx_array_push(arr, jx_string("x")), 0);
  ASSERT_EQ_INT(jx_object_set(obj, "a", arr), 0); /* obj owns arr now */
  ASSERT_EQ_INT(jx_object_set(obj, "b", jx_bool(1)), 0);
  ASSERT_EQ_INT(jx_object_set(obj, "c", jx_null()), 0);
  ASSERT_EQ_INT(jx_object_count(obj), 3);
  ASSERT_TRUE(jx_object_get(obj, "b") != NULL);
  ASSERT_TRUE(jx_object_get(obj, "zz") == NULL);
  out = jx_write(obj, 0);
  ASSERT_TRUE(out != NULL);
  ASSERT_EQ_STR(out, "{\"a\":[1,\"x\"],\"b\":true,\"c\":null}");
  free(out);
  /* NaN/Inf have no JSON representation */
  {
    jx_value *zero = jx_double(0.0);
    jx_value *nan = jx_double((double)NAN);
    ASSERT_TRUE(zero != NULL);
    ASSERT_TRUE(nan == NULL);
    jx_free(zero);
  }
  {
    jx_value *clone = jx_clone(obj);
    char *out2;
    ASSERT_TRUE(clone != NULL);
    out2 = jx_write(clone, 0);
    ASSERT_TRUE(out2 != NULL);
    ASSERT_EQ_STR(out2, "{\"a\":[1,\"x\"],\"b\":true,\"c\":null}");
    free(out2);
    jx_free(clone);
  }
  jx_free(obj);
}

TEST_LIST = {
  TEST_ENTRY(round_trips),
  TEST_ENTRY(escapes),
  TEST_ENTRY(surrogate_pairs),
  TEST_ENTRY(depth_cap),
  TEST_ENTRY(trailing_garbage_rejected),
  TEST_ENTRY(strictness_rejects),
  TEST_ENTRY(number_views),
  TEST_ENTRY(constructors_and_containers),
};

RUN_ALL_TESTS()
