/*
 * test_regex.c — std/regex.c: in-house linear-time ERE subset. Compiled
 * against std/regex.c directly through std/sdk.h.
 */

#include "astools_test.h"

#include "sdk.h"

/* Compile + search; returns 1 on match filling *s / *e. -1 = compile
 * error. */
static int try_match(const char *pattern, int ci, const char *line,
                     size_t *s, size_t *e) {
  const char *err = NULL;
  astd_regex *re = astd_regex_compile(pattern, !ci ? 1 : 0, &err);
  int r;
  if (!re) return -1;
  r = astd_regex_search(re, line, strlen(line), s, e);
  astd_regex_free(re);
  return r;
}

/* Expect a match at [ws, we). */
static void m(const char *pattern, const char *line, long ws, long we) {
  size_t s = (size_t)-1, e = (size_t)-1;
  int r = try_match(pattern, 0, line, &s, &e);
  if (r == -1) {
    ASTOOLS_FAILF("compile failed: /%s/", pattern);
    return;
  }
  if (r != 1) {
    ASTOOLS_FAILF("/%s/ did not match '%s'", pattern, line);
    return;
  }
  if ((long)s != ws || (long)e != we)
    ASTOOLS_FAILF("/%s/ on '%s': [%ld,%ld), want [%ld,%ld)", pattern, line,
                (long)s, (long)e, ws, we);
}

/* Expect no match. */
static void nm(const char *pattern, const char *line) {
  size_t s, e;
  int r = try_match(pattern, 0, line, &s, &e);
  if (r == -1) {
    ASTOOLS_FAILF("compile failed: /%s/", pattern);
    return;
  }
  if (r != 0) ASTOOLS_FAILF("/%s/ unexpectedly matched '%s'", pattern, line);
}

/* Expect a compile-time rejection with a diagnostic message. */
static void bad(const char *pattern) {
  const char *err = NULL;
  astd_regex *re = astd_regex_compile(pattern, 1, &err);
  if (re != NULL) {
    ASTOOLS_FAILF("compile accepted unsupported /%s/", pattern);
    astd_regex_free(re);
    return;
  }
  if (err == NULL || err[0] == '\0')
    ASTOOLS_FAILF("/%s/ rejected without a message", pattern);
}

TEST(literals_and_dot) {
  m("abc", "xxabcxx", 2, 5);
  m("a", "a", 0, 1);
  nm("abc", "abx");
  m("a.c", "abc", 0, 3);
  m("a.c", "a.c", 0, 3);
  nm("a.c", "ac");
}

TEST(classes_and_ranges) {
  m("[abc]", "zbz", 1, 2);
  nm("[abc]", "zzz");
  m("[a-f]+", "zzdeadbeefzz", 2, 10);
  m("[0-9][0-9]", "ab42cd", 2, 4);
  m("[-a]", "-", 0, 1);   /* literal '-' at class edge */
  nm("[^abc]", "ab");
  m("[^abc]", "abX", 2, 3);
  m("[a-cx-z]", "y", 0, 1);
  nm("[a-cx-z]", "m");
}

TEST(anchors) {
  m("^abc", "abcdef", 0, 3);
  nm("^abc", "xabc");
  m("abc$", "xxabc", 2, 5);
  nm("abc$", "abcx");
  m("^abc$", "abc", 0, 3);
  nm("^abc$", "abcd");
  m("^$", "", 0, 0);
  m("^", "xy", 0, 0);
}

TEST(quantifiers) {
  m("ab*c", "ac", 0, 2);
  m("ab*c", "abbbc", 0, 5);
  m("ab+c", "abc", 0, 3);
  m("ab+c", "abbc", 0, 4);
  nm("ab+c", "ac");
  m("ab?c", "ac", 0, 2);
  m("ab?c", "abc", 0, 3);
  nm("ab?c", "abbc");
  /* leftmost-longest within the subset engine */
  m("a+", "baaa", 1, 4);
}

TEST(bounded_repetition) {
  m("a{2,3}", "aaaa", 0, 3);
  m("a{2,3}", "aa", 0, 2);
  nm("a{2,3}", "a");
  m("a{3}", "aaa", 0, 3);
  nm("a{3}", "aa");
  m("a{2,}", "aaaaa", 0, 5);
  nm("a{2,}", "a");
  m("(ab){2}", "xababx", 1, 5);
}

TEST(alternation_and_groups) {
  m("cat|dog", "hotdog", 3, 6);
  m("cat|dog", "catalog", 0, 3);
  nm("cat|dog", "bird");
  m("gr(a|e)y", "grey", 0, 4);
  m("gr(a|e)y", "gray", 0, 4);
  nm("gr(a|e)y", "groy");
  m("(ab|cd)+", "zabcdz", 1, 5);
  m("a(b(c|d))e", "abde", 0, 4);
}

TEST(escapes) {
  m("a\\.c", "a.c", 0, 3);
  nm("a\\.c", "abc");
  m("\\[x\\]", "[x]", 0, 3);
  m("a\\\\b", "a\\b", 0, 3);
  m("\\(\\)", "()", 0, 2);
  m("\\{2\\}", "{2}", 0, 3);
  m("a\\|b", "a|b", 0, 3);
  m("\\^\\$", "^$", 0, 2);
}

TEST(case_insensitive_flag) {
  size_t s = 0, e = 0;
  ASSERT_EQ_INT(try_match("abc", 1, "xxABCxx", &s, &e), 1);
  ASSERT_EQ_INT((long)s, 2);
  ASSERT_EQ_INT((long)e, 5);
  ASSERT_EQ_INT(try_match("[a-f]+", 1, "DEADBEEF", &s, &e), 1);
  ASSERT_EQ_INT(try_match("AbC", 1, "aBc", &s, &e), 1);
  /* and the case-sensitive engine stays exact */
  ASSERT_EQ_INT(try_match("abc", 0, "ABC", &s, &e), 0);
}

TEST(rejects_unsupported_constructs) {
  bad("(a)\\1");   /* backreference */
  bad("a(?=b)");   /* lookahead */
  bad("a(?!b)");
  bad("(?:a)");    /* any (?… group syntax */
  bad("(a");       /* unbalanced */
  bad("a)");
  bad("[a");       /* unterminated class */
  bad("a{2");      /* unterminated bound */
  bad("a{3,2}");   /* inverted bound */
  bad("*a");       /* nothing to repeat */
}

TEST(pathological_pattern_is_linear) {
  /* (a|a|a|a)*b against 'aaaa…' explodes exponentially on a backtracker;
   * the linear engine must finish instantly (CTest TIMEOUT 60 is the
   * hard backstop). ~2KB input per guidance. */
  char input[2049];
  size_t s, e;
  const char *err = NULL;
  astd_regex *re;
  int r;
  memset(input, 'a', sizeof input - 1);
  input[sizeof input - 1] = '\0';
  re = astd_regex_compile("(a|a|a|a)*b", 1, &err);
  if (!re) {
    ASTOOLS_FAILF("compile failed: %s", err ? err : "(no message)");
    return;
  }
  r = astd_regex_search(re, input, sizeof input - 1, &s, &e);
  ASSERT_EQ_INT(r, 0); /* no 'b' anywhere */
  input[sizeof input - 2] = 'b';
  r = astd_regex_search(re, input, sizeof input - 1, &s, &e);
  astd_regex_free(re);
  ASSERT_EQ_INT(r, 1);
}

TEST(searches_are_binary_safe) {
  /* length-delimited search: an embedded NUL is a normal byte */
  const char line[] = "ab\0cd";
  const char *err = NULL;
  astd_regex *re = astd_regex_compile("cd", 1, &err);
  size_t s = 0, e = 0;
  int r;
  if (!re) {
    ASTOOLS_FAILF("compile failed");
    return;
  }
  r = astd_regex_search(re, line, 5, &s, &e);
  astd_regex_free(re);
  ASSERT_EQ_INT(r, 1);
  ASSERT_EQ_INT((long)s, 3);
  ASSERT_EQ_INT((long)e, 5);
}

TEST_LIST = {
  TEST_ENTRY(literals_and_dot),
  TEST_ENTRY(classes_and_ranges),
  TEST_ENTRY(anchors),
  TEST_ENTRY(quantifiers),
  TEST_ENTRY(bounded_repetition),
  TEST_ENTRY(alternation_and_groups),
  TEST_ENTRY(escapes),
  TEST_ENTRY(case_insensitive_flag),
  TEST_ENTRY(rejects_unsupported_constructs),
  TEST_ENTRY(pathological_pattern_is_linear),
  TEST_ENTRY(searches_are_binary_safe),
};

RUN_ALL_TESTS()
