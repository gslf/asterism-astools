/*
 * test_path.c — path.c: canonicalization + prefix containment.
 * Includes the classic symlink-escape detection case.
 */

#include "astools_test.h"

#include "astools_internal.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

/* Fixture: ws (canonical), ws/a/b directory chain. */
typedef struct {
  char raw[256];
  char *ws; /* canonical (realpath) — differs from raw on macOS /tmp */
} ws_fixture;

static int ws_make(ws_fixture *f) {
  char sub[512];
  f->ws = NULL;
  if (!astools_test_tmpdir(f->raw)) return 0;
  snprintf(sub, sizeof sub, "%s/a/b", f->raw);
  if (os_mkdir_p(sub) != ASTOOLS_OK) return 0;
  if (os_realpath(f->raw, &f->ws) != ASTOOLS_OK) return 0;
  return 1;
}

static void ws_drop(ws_fixture *f) {
  astools_test_rmtree(f->raw);
  free(f->ws);
  f->ws = NULL;
}

/* Expected = canonical ws + "/" + rel. */
static int expect_path(const ws_fixture *f, const char *got,
                       const char *rel) {
  char want[1024];
  snprintf(want, sizeof want, "%s/%s", f->ws, rel);
  return got != NULL && strcmp(got, want) == 0;
}

TEST(canonical_join_relative) {
  ws_fixture f;
  char *out = NULL;
  if (!ws_make(&f)) {
    ASTOOLS_FAILF("fixture setup failed");
    ws_drop(&f);
    return;
  }
  if (astools_path_canonical(f.ws, "a", false, &out) != ASTOOLS_OK ||
      !expect_path(&f, out, "a")) {
    ASTOOLS_FAILF("canonical(ws, \"a\") = %s", out ? out : "(null)");
    free(out);
    ws_drop(&f);
    return;
  }
  free(out);
  out = NULL;
  /* absolute input passes through canonicalization too */
  if (astools_path_canonical(f.ws, f.raw, false, &out) != ASTOOLS_OK ||
      strcmp(out, f.ws) != 0) {
    ASTOOLS_FAILF("canonical(abs ws) = %s, want %s", out ? out : "(null)",
                f.ws);
    free(out);
    ws_drop(&f);
    return;
  }
  free(out);
  ws_drop(&f);
}

TEST(canonical_dot_segments_existing) {
  ws_fixture f;
  char *out = NULL;
  if (!ws_make(&f)) {
    ASTOOLS_FAILF("fixture setup failed");
    ws_drop(&f);
    return;
  }
  /* "." and ".." resolve through EXISTING directories via the kernel */
  if (astools_path_canonical(f.ws, "a/./b", false, &out) != ASTOOLS_OK ||
      !expect_path(&f, out, "a/b")) {
    ASTOOLS_FAILF("a/./b -> %s", out ? out : "(null)");
    free(out);
    ws_drop(&f);
    return;
  }
  free(out);
  out = NULL;
  if (astools_path_canonical(f.ws, "a/../a/b", false, &out) != ASTOOLS_OK ||
      !expect_path(&f, out, "a/b")) {
    ASTOOLS_FAILF("a/../a/b -> %s", out ? out : "(null)");
    free(out);
    ws_drop(&f);
    return;
  }
  free(out);
  ws_drop(&f);
}

TEST(canonical_missing_tail) {
  ws_fixture f;
  char *out = NULL;
  astools_err e;
  if (!ws_make(&f)) {
    ASTOOLS_FAILF("fixture setup failed");
    ws_drop(&f);
    return;
  }
  /* missing tail re-appended after canonicalizing the deepest ancestor */
  e = astools_path_canonical(f.ws, "a/missing/new.txt", true, &out);
  if (e != ASTOOLS_OK || !expect_path(&f, out, "a/missing/new.txt")) {
    ASTOOLS_FAILF("missing tail -> %s (%s)", out ? out : "(null)",
                astools_err_name(e));
    free(out);
    ws_drop(&f);
    return;
  }
  free(out);
  out = NULL;
  /* same path with missing_ok = false must report NOT_FOUND */
  ASSERT_ERR(astools_path_canonical(f.ws, "a/missing/new.txt", false, &out),
             ASTOOLS_ERR_NOT_FOUND);
  ASSERT_TRUE(out == NULL);
  ws_drop(&f);
}

TEST(canonical_rejects_dotdot_in_missing_tail) {
  /* SAFETY: ".." in a non-existing tail would be re-appended textually and
   * escape the canonical ancestor — must be rejected outright. */
  ws_fixture f;
  char *out = NULL;
  astools_err e;
  if (!ws_make(&f)) {
    ASTOOLS_FAILF("fixture setup failed");
    ws_drop(&f);
    return;
  }
  e = astools_path_canonical(f.ws, "a/missing/../x", true, &out);
  if (e == ASTOOLS_OK) {
    ASTOOLS_FAILF("'..' in missing tail accepted: %s", out ? out : "(null)");
    free(out);
    ws_drop(&f);
    return;
  }
  ASSERT_TRUE(out == NULL);
  e = astools_path_canonical(f.ws, "a/missing/./x", true, &out);
  if (e == ASTOOLS_OK) {
    ASTOOLS_FAILF("'.' in missing tail accepted: %s", out ? out : "(null)");
    free(out);
    ws_drop(&f);
    return;
  }
  ws_drop(&f);
}

TEST(canonical_invalid_inputs) {
  char *out = NULL;
  ASSERT_ERR(astools_path_canonical("/tmp", "", false, &out),
             ASTOOLS_ERR_INVALID);
  ASSERT_ERR(astools_path_canonical("/tmp", NULL, false, &out),
             ASTOOLS_ERR_INVALID);
  ASSERT_ERR(astools_path_canonical(NULL, "x", false, NULL),
             ASTOOLS_ERR_INVALID);
}

#if !defined(_WIN32)
TEST(symlink_escape_detected) {
  /* The classic attack: A/link -> B, so A/link/x really lives under B.
   * Canonicalization must expose the real location. */
  char raw_a[256], raw_b[256];
  char link_path[512], target[512];
  char *canon_a = NULL, *canon_b = NULL, *out = NULL;
  FILE *fp;
  astools_err e;

  if (!astools_test_tmpdir(raw_a) || !astools_test_tmpdir(raw_b)) {
    ASTOOLS_FAILF("tmpdir setup failed");
    return;
  }
  snprintf(link_path, sizeof link_path, "%s/link", raw_a);
  snprintf(target, sizeof target, "%s/x", raw_b);
  fp = fopen(target, "wb");
  if (!fp || symlink(raw_b, link_path) != 0) {
    if (fp) fclose(fp);
    ASTOOLS_FAILF("symlink fixture failed");
    astools_test_rmtree(raw_a);
    astools_test_rmtree(raw_b);
    return;
  }
  fputs("x", fp);
  fclose(fp);

  if (os_realpath(raw_a, &canon_a) != ASTOOLS_OK ||
      os_realpath(raw_b, &canon_b) != ASTOOLS_OK) {
    ASTOOLS_FAILF("realpath fixture failed");
    goto out;
  }

  e = astools_path_canonical(canon_a, "link/x", false, &out);
  if (e != ASTOOLS_OK || out == NULL) {
    ASTOOLS_FAILF("canonical(A, link/x) failed: %s", astools_err_name(e));
    goto out;
  }
  /* resolved under B, NOT under A */
  ASSERT_TRUE(astools_path_under(out, canon_b) == 1);
  ASSERT_TRUE(astools_path_under(out, canon_a) == 0);

out:
  free(out);
  free(canon_a);
  free(canon_b);
  astools_test_rmtree(raw_a);
  astools_test_rmtree(raw_b);
}
#endif /* !_WIN32 */

TEST(path_under_boundaries) {
  ASSERT_EQ_INT(astools_path_under("/a/b", "/a"), 1);
  ASSERT_EQ_INT(astools_path_under("/a", "/a"), 1);   /* equal counts */
  ASSERT_EQ_INT(astools_path_under("/a", "/a/"), 1);  /* trailing slash */
  ASSERT_EQ_INT(astools_path_under("/ab", "/a"), 0);  /* component bound */
  ASSERT_EQ_INT(astools_path_under("/a", "/ab"), 0);
  ASSERT_EQ_INT(astools_path_under("/a/bc", "/a/b"), 0);
  ASSERT_EQ_INT(astools_path_under("/anything/at/all", "/"), 1);
  ASSERT_EQ_INT(astools_path_under("/", "/"), 1);
  ASSERT_EQ_INT(astools_path_under("relative", "/a"), 0);
  ASSERT_EQ_INT(astools_path_under("/a", "relative"), 0);
  ASSERT_EQ_INT(astools_path_under(NULL, "/a"), 0);
  ASSERT_EQ_INT(astools_path_under("/a", NULL), 0);
}

TEST_LIST = {
  TEST_ENTRY(canonical_join_relative),
  TEST_ENTRY(canonical_dot_segments_existing),
  TEST_ENTRY(canonical_missing_tail),
  TEST_ENTRY(canonical_rejects_dotdot_in_missing_tail),
  TEST_ENTRY(canonical_invalid_inputs),
#if !defined(_WIN32)
  TEST_ENTRY(symlink_escape_detected),
#endif
  TEST_ENTRY(path_under_boundaries),
};

RUN_ALL_TESTS()
