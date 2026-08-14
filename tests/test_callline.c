/*
 * test_callline.c — callline.c: CALL parsing + RESULT/ERROR formatting
 * (§7.2, §17).
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "xcdn.h"

/* Parse out_args (xCDN object text) and return the string value at key. */
static char *args_key_string(const char *args_text, const char *key) {
  xcdn_error_t xe;
  xcdn_document_t *doc;
  const xcdn_node_t *n;
  char *out = NULL;
  memset(&xe, 0, sizeof xe);
  doc = xcdn_parse(args_text, &xe);
  if (!doc || doc->values_len == 0) {
    if (doc) xcdn_document_free(doc);
    return NULL;
  }
  n = doc->values[0];
  if (n && n->value && n->value->type == XCDN_VAL_OBJECT) {
    const xcdn_node_t *v = xcdn_object_get(n->value, key);
    if (v && v->value && v->value->type == XCDN_VAL_STRING &&
        v->value->data.string)
      out = astools_strdup(v->value->data.string);
  }
  xcdn_document_free(doc);
  return out;
}

static void free3(char *a, char *b, char *c) {
  free(a);
  free(b);
  free(c);
}

TEST(parse_happy_path) {
  char *ref = NULL, *cmd = NULL, *args = NULL, *path;
  ASSERT_OK(astools_callline_parse(
      "CALL fs.read {path: \"notes/todo.txt\"}", &ref, &cmd, &args));
  ASSERT_EQ_STR(ref, "fs");
  ASSERT_EQ_STR(cmd, "read");
  ASSERT_TRUE(args != NULL && args[0] == '{');
  path = args_key_string(args, "path");
  ASSERT_EQ_STR(path, "notes/todo.txt");
  free(path);
  free3(ref, cmd, args);
}

TEST(parse_version_ref) {
  /* `CALL <tool>[@<version>].<command> { … }` — the version itself
   * contains dots, so the command is the LAST dot-separated segment. */
  char *ref = NULL, *cmd = NULL, *args = NULL;
  ASSERT_OK(astools_callline_parse("CALL fs@1.2.0.read {path: \"x\"}", &ref,
                                 &cmd, &args));
  ASSERT_EQ_STR(ref, "fs@1.2.0");
  ASSERT_EQ_STR(cmd, "read");
  free3(ref, cmd, args);
}

TEST(parse_skips_prose) {
  char *ref = NULL, *cmd = NULL, *args = NULL;
  ASSERT_OK(astools_callline_parse(
      "I will now read the file for you.\n"
      "Let me use the fs tool.\n"
      "CALL fs.read {path: \"a\"}\n"
      "That should do it.",
      &ref, &cmd, &args));
  ASSERT_EQ_STR(ref, "fs");
  ASSERT_EQ_STR(cmd, "read");
  free3(ref, cmd, args);
}

TEST(parse_malformed_skipped_then_valid_taken) {
  /* first WELL-FORMED call line wins (§7.2) */
  char *ref = NULL, *cmd = NULL, *args = NULL, *path;
  ASSERT_OK(astools_callline_parse(
      "CALL fs.read no braces here\n"
      "CALL not-even-a-command\n"
      "CALL fs.read {path: \"ok\"}\n",
      &ref, &cmd, &args));
  ASSERT_EQ_STR(ref, "fs");
  ASSERT_EQ_STR(cmd, "read");
  path = args_key_string(args, "path");
  ASSERT_EQ_STR(path, "ok");
  free(path);
  free3(ref, cmd, args);
}

TEST(parse_braces_inside_strings) {
  char *ref = NULL, *cmd = NULL, *args = NULL, *v;
  ASSERT_OK(astools_callline_parse(
      "CALL fs.write {path: \"a}b{c\", content: \"x{\"}", &ref, &cmd,
      &args));
  ASSERT_EQ_STR(ref, "fs");
  ASSERT_EQ_STR(cmd, "write");
  v = args_key_string(args, "path");
  ASSERT_EQ_STR(v, "a}b{c");
  free(v);
  v = args_key_string(args, "content");
  ASSERT_EQ_STR(v, "x{");
  free(v);
  free3(ref, cmd, args);
}

TEST(parse_first_call_wins) {
  char *ref = NULL, *cmd = NULL, *args = NULL;
  ASSERT_OK(astools_callline_parse(
      "CALL grep.search {pattern: \"a\"}\n"
      "CALL fs.read {path: \"b\"}\n",
      &ref, &cmd, &args));
  ASSERT_EQ_STR(ref, "grep");
  ASSERT_EQ_STR(cmd, "search");
  free3(ref, cmd, args);
}

TEST(parse_not_found) {
  char *ref = NULL, *cmd = NULL, *args = NULL;
  ASSERT_ERR(astools_callline_parse("no calls here, just prose.\n", &ref,
                                  &cmd, &args),
             ASTOOLS_ERR_NOT_FOUND);
  ASSERT_TRUE(ref == NULL && cmd == NULL && args == NULL);
  ASSERT_ERR(astools_callline_parse("", &ref, &cmd, &args),
             ASTOOLS_ERR_NOT_FOUND);
}

TEST(format_result_line) {
  /* result_xcdn is already compact xCDN text; the RESULT line carries it
   * verbatim after "RESULT <ref>.<command> " (§7.2). */
  astools_result r;
  char *line = NULL;
  memset(&r, 0, sizeof r);
  r.ok = 1;
  r.result_xcdn = (char *)"{n: 1}";
  ASSERT_OK(astools_callline_format("fs", "read", &r, &line));
  ASSERT_EQ_STR(line, "RESULT fs.read {n: 1}");
  free(line);
}

TEST(format_error_line) {
  /* contract: §7.2 — ERROR <tool>.<command> {code: …, message: …}; the
   * object body is compact xCDN, so the expectation is built with the
   * same serializer instead of a hand-written literal. */
  astools_result r;
  char *line = NULL, *want_obj = NULL;
  astools_buf want;
  xcdn_error_t xe;
  xcdn_document_t *doc;

  memset(&r, 0, sizeof r);
  r.ok = 0;
  r.error_code = (char *)"fs/not-found";
  r.error_message = (char *)"no such file: \"a\"";

  memset(&xe, 0, sizeof xe);
  doc = xcdn_parse("{code: \"fs/not-found\","
                   " message: \"no such file: \\\"a\\\"\"}",
                   &xe);
  if (!doc || doc->values_len == 0) {
    ASTOOLS_FAILF("expectation fixture failed to parse");
    if (doc) xcdn_document_free(doc);
    return;
  }
  want_obj = astools_xcdn_write_value(doc->values[0]->value, false);
  xcdn_document_free(doc);
  if (!want_obj) {
    ASTOOLS_FAILF("expectation serialization failed");
    return;
  }
  astools_buf_init(&want);
  if (astools_buf_printf(&want, "ERROR fs.read %s", want_obj) !=
      ASTOOLS_OK) {
    ASTOOLS_FAILF("out of memory");
    free(want_obj);
    return;
  }

  ASSERT_OK(astools_callline_format("fs", "read", &r, &line));
  ASSERT_EQ_STR(line, want.data);
  free(line);
  free(want_obj);
  astools_buf_free(&want);
}

TEST(format_line_is_single_line) {
  /* newlines in the message must be escaped into the string literal */
  astools_result r;
  char *line = NULL;
  memset(&r, 0, sizeof r);
  r.ok = 0;
  r.error_code = (char *)"fs/io";
  r.error_message = (char *)"line1\nline2";
  ASSERT_OK(astools_callline_format("fs", "read", &r, &line));
  ASSERT_TRUE(strchr(line, '\n') == NULL);
  ASSERT_TRUE(strstr(line, "\\n") != NULL);
  free(line);
}

TEST_LIST = {
  TEST_ENTRY(parse_happy_path),
  TEST_ENTRY(parse_version_ref),
  TEST_ENTRY(parse_skips_prose),
  TEST_ENTRY(parse_malformed_skipped_then_valid_taken),
  TEST_ENTRY(parse_braces_inside_strings),
  TEST_ENTRY(parse_first_call_wins),
  TEST_ENTRY(parse_not_found),
  TEST_ENTRY(format_result_line),
  TEST_ENTRY(format_error_line),
  TEST_ENTRY(format_line_is_single_line),
};

RUN_ALL_TESTS()
