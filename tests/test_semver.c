/*
 * test_semver.c — semver.c: SemVer 2.0.0 parse + precedence.
 */

#include "astools_test.h"

#include "astools_internal.h"

static int cmp_vers(const char *a, const char *b) {
  astools_semver va, vb;
  int r;
  memset(&va, 0, sizeof va);
  memset(&vb, 0, sizeof vb);
  if (!astools_semver_parse(a, &va)) return -99;
  if (!astools_semver_parse(b, &vb)) {
    astools_semver_free(&va);
    return -99;
  }
  r = astools_semver_cmp(&va, &vb);
  astools_semver_free(&va);
  astools_semver_free(&vb);
  return r;
}

TEST(parse_fields) {
  astools_semver v;
  memset(&v, 0, sizeof v);
  ASSERT_TRUE(astools_semver_parse("1.2.3", &v));
  ASSERT_EQ_INT(v.major, 1);
  ASSERT_EQ_INT(v.minor, 2);
  ASSERT_EQ_INT(v.patch, 3);
  ASSERT_TRUE(v.prerelease == NULL);
  astools_semver_free(&v);

  memset(&v, 0, sizeof v);
  ASSERT_TRUE(astools_semver_parse("10.20.30-alpha.1+build.5", &v));
  ASSERT_EQ_INT(v.major, 10);
  ASSERT_EQ_INT(v.minor, 20);
  ASSERT_EQ_INT(v.patch, 30);
  ASSERT_EQ_STR(v.prerelease, "alpha.1"); /* build metadata dropped */
  astools_semver_free(&v);

  memset(&v, 0, sizeof v);
  ASSERT_TRUE(astools_semver_parse("0.0.0", &v));
  ASSERT_EQ_INT(v.major, 0);
  astools_semver_free(&v);
}

TEST(parse_rejects) {
  astools_semver v;
  static const char *bad[] = {
    "",        "1",         "1.0",       "1.0.0.0",  "v1.0.0",
    "01.0.0",  "1.02.0",    "1.0.03",    /* leading zeros */
    "1.0.0-",  "1.0.0+",    "1.0.0-a..b", "1.0.0-a_b",
    "1.0.0-01",              /* numeric prerelease id leading zero */
    "1.0.0-alpha.01",
    "1.0.-1",  "-1.0.0",    "1.0.0 ",    " 1.0.0",   "1..0",
    "a.b.c",   "1.0.0-+x",
  };
  size_t i;
  for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
    memset(&v, 0, sizeof v);
    if (astools_semver_parse(bad[i], &v)) {
      ASTOOLS_FAILF("accepted invalid semver '%s'", bad[i]);
      astools_semver_free(&v);
      return;
    }
  }
  memset(&v, 0, sizeof v);
  ASSERT_TRUE(!astools_semver_parse(NULL, &v));
}

TEST(precedence_table) {
  /* SemVer 2.0.0 canonical ordering chain. */
  static const char *chain[] = {
    "1.0.0-alpha",     "1.0.0-alpha.1", "1.0.0-alpha.beta",
    "1.0.0-beta",      "1.0.0-beta.2",  "1.0.0-beta.11",
    "1.0.0-rc.1",      "1.0.0",
  };
  size_t i, j;
  for (i = 0; i < sizeof chain / sizeof chain[0]; i++) {
    for (j = 0; j < sizeof chain / sizeof chain[0]; j++) {
      int r = cmp_vers(chain[i], chain[j]);
      int want = (i < j) ? -1 : (i > j) ? 1 : 0;
      if (r == -99) {
        ASTOOLS_FAILF("parse failed for '%s' / '%s'", chain[i], chain[j]);
        return;
      }
      if ((want < 0 && r >= 0) || (want > 0 && r <= 0) ||
          (want == 0 && r != 0)) {
        ASTOOLS_FAILF("cmp('%s','%s') = %d, want sign %d", chain[i], chain[j],
                    r, want);
        return;
      }
    }
  }
}

TEST(numeric_component_ordering) {
  ASSERT_TRUE(cmp_vers("1.0.0", "2.0.0") < 0);
  ASSERT_TRUE(cmp_vers("2.0.0", "2.1.0") < 0);
  ASSERT_TRUE(cmp_vers("2.1.0", "2.1.1") < 0);
  ASSERT_TRUE(cmp_vers("1.9.0", "1.10.0") < 0);  /* numeric, not lexical */
  ASSERT_TRUE(cmp_vers("1.0.0-2", "1.0.0-11") < 0);
  ASSERT_TRUE(cmp_vers("1.0.0-1", "1.0.0-a") < 0); /* numeric < alpha */
  ASSERT_TRUE(cmp_vers("1.0.0-a", "1.0.0-a.1") < 0); /* prefix < longer */
}

TEST(build_metadata_ignored) {
  ASSERT_EQ_INT(cmp_vers("1.0.0+build1", "1.0.0+build2"), 0);
  ASSERT_EQ_INT(cmp_vers("1.0.0+x", "1.0.0"), 0);
  ASSERT_EQ_INT(cmp_vers("1.0.0-alpha+001", "1.0.0-alpha"), 0);
  ASSERT_TRUE(cmp_vers("1.0.0-alpha+zzz", "1.0.0") < 0);
}

TEST_LIST = {
  TEST_ENTRY(parse_fields),
  TEST_ENTRY(parse_rejects),
  TEST_ENTRY(precedence_table),
  TEST_ENTRY(numeric_component_ordering),
  TEST_ENTRY(build_metadata_ignored),
};

RUN_ALL_TESTS()
