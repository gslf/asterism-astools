/*
 * test_escape.c — basic-tier honesty suite (§6, §17): every escape the
 * pre-flight claims to stop must be stopped BEFORE anything is spawned,
 * and a timed-out child must not poison the context. Hermetic: tmpdirs
 * only, no network.
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "fakes.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef ASTOOLS_TEST_TOOL_PATH
#define ASTOOLS_TEST_TOOL_PATH ""
#endif

/* Compile definition first (always a real path when CMake set it); the
 * ASTOOLS_TEST_TOOL env var is the manual-run fallback. */
static const char *tool_path(void) {
  static const char defined[] = ASTOOLS_TEST_TOOL_PATH;
  const char *env;
  if (defined[0] != '\0') return defined;
  env = getenv("ASTOOLS_TEST_TOOL");
  return env != NULL ? env : "";
}

typedef struct {
  char ws_raw[256], root_raw[256], out_raw[256], cfg_raw[256];
  char cfg_path[512];
  char *ws, *outside; /* canonical */
  astools_ctx *c;
} esc_fx;

static int esc_setup(esc_fx *f) {
  astools_open_params p;
  memset(f, 0, sizeof *f);
  if (tool_path()[0] == '\0') return 0;
  if (!astools_test_tmpdir(f->ws_raw) || !astools_test_tmpdir(f->root_raw) ||
      !astools_test_tmpdir(f->out_raw) || !astools_test_tmpdir(f->cfg_raw))
    return 0;
  if (os_realpath(f->ws_raw, &f->ws) != ASTOOLS_OK) return 0;
  if (os_realpath(f->out_raw, &f->outside) != ASTOOLS_OK) return 0;

  if (!fake_registry_write(f->root_raw, "fk", tool_path(), "echo", NULL,
                           NULL, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "slp", tool_path(), "sleep", NULL,
                           FAKE_CMD_RUN_MS, NULL))
    return 0;

  snprintf(f->cfg_path, sizeof f->cfg_path, "%s/config.xcdn", f->cfg_raw);
  if (!fake_config_write(f->cfg_path, f->root_raw, 1, f->ws, NULL)) return 0;

  memset(&p, 0, sizeof p);
  p.config_path = f->cfg_path;
  return astools_open(&p, &f->c) == ASTOOLS_OK;
}

static void esc_drop(esc_fx *f) {
  if (f->c) astools_close(f->c);
  free(f->ws);
  free(f->outside);
  astools_test_rmtree(f->ws_raw);
  astools_test_rmtree(f->root_raw);
  astools_test_rmtree(f->out_raw);
  astools_test_rmtree(f->cfg_raw);
}

/* Invoke fk.run with the given p value; expect ERR_DENIED and a denied
 * counter bump (the fake tool must never run). */
static void expect_denied(esc_fx *f, const char *p_literal) {
  astools_result r;
  astools_stats before, after;
  char args[1200];
  astools_err e;
  memset(&r, 0, sizeof r);
  if (astools_get_stats(f->c, &before) != ASTOOLS_OK) {
    ASTOOLS_FAILF("get_stats failed");
    return;
  }
  snprintf(args, sizeof args, "{ msg: \"x\", p: \"%s\" }", p_literal);
  e = astools_invoke(f->c, "fk", "run", args, 0, &r);
  if (e != ASTOOLS_ERR_DENIED) {
    ASTOOLS_FAILF("p=%s: expected ERR_DENIED, got %s (%s)", p_literal,
                astools_err_name(e), astools_last_error(f->c));
    astools_result_free(&r);
    return;
  }
  astools_result_free(&r);
  if (astools_get_stats(f->c, &after) != ASTOOLS_OK) {
    ASTOOLS_FAILF("get_stats failed");
    return;
  }
  if (after.denied <= before.denied)
    ASTOOLS_FAILF("p=%s: stats.denied did not advance", p_literal);
}

TEST(relative_traversal_denied_before_spawn) {
  esc_fx f;
  char marker[512], sub[600];
  if (!esc_setup(&f)) {
    ASTOOLS_FAILF("setup failed (is ASTOOLS_TEST_TOOL_PATH set?)");
    esc_drop(&f);
    return;
  }
  expect_denied(&f, "../outside.txt");
  /* traversal through an EXISTING subdirectory (kernel-resolved "..") */
  snprintf(sub, sizeof sub, "%s/sub", f.ws);
  if (os_mkdir_p(sub) != ASTOOLS_OK) {
    ASTOOLS_FAILF("mkdir fixture failed");
    esc_drop(&f);
    return;
  }
  expect_denied(&f, "sub/../../outside.txt");
  /* nothing appeared next to the workspace */
  snprintf(marker, sizeof marker, "%s/../outside.txt", f.ws);
  ASSERT_TRUE(!os_file_exists(marker));
  esc_drop(&f);
}

TEST(absolute_path_outside_workspace_denied) {
  esc_fx f;
  char abs_target[600];
  if (!esc_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    esc_drop(&f);
    return;
  }
  snprintf(abs_target, sizeof abs_target, "%s/steal.txt", f.outside);
  expect_denied(&f, abs_target);
  esc_drop(&f);
}

#if !defined(_WIN32)
TEST(symlink_inside_workspace_pointing_outside_denied) {
  /* ws/link -> outside; ws-relative "link/steal.txt" canonicalizes to the
   * outside tree, which the grants do not cover (§6.4). */
  esc_fx f;
  char link_path[600], rel[64];
  if (!esc_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    esc_drop(&f);
    return;
  }
  snprintf(link_path, sizeof link_path, "%s/link", f.ws);
  if (symlink(f.outside, link_path) != 0) {
    ASTOOLS_FAILF("symlink fixture failed");
    esc_drop(&f);
    return;
  }
  snprintf(rel, sizeof rel, "link/steal.txt");
  expect_denied(&f, rel);
  esc_drop(&f);
}
#endif /* !_WIN32 */

TEST(write_to_new_file_under_workspace_allowed) {
  /* The honest counterpart: a write-typed path to a NEW file under the
   * workspace passes pre-flight and reaches the tool canonicalized. */
  esc_fx f;
  astools_result r;
  char want[1024];
  memset(&r, 0, sizeof r);
  if (!esc_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    esc_drop(&f);
    return;
  }
  if (astools_invoke(f.c, "fk", "run",
                     "{ msg: \"w\", p: \"newdir/new.txt\" }", 0,
                     &r) != ASTOOLS_OK) {
    ASTOOLS_FAILF("in-workspace write path denied: %s",
                astools_last_error(f.c));
    esc_drop(&f);
    return;
  }
  ASSERT_EQ_INT(r.ok, 1);
  snprintf(want, sizeof want, "%s/newdir/new.txt", f.ws);
  ASSERT_TRUE(r.result_xcdn != NULL);
  if (strstr(r.result_xcdn, want) == NULL)
    ASTOOLS_FAILF("result lacks the canonical path %s: %s", want,
                r.result_xcdn);
  astools_result_free(&r);
  esc_drop(&f);
}

TEST(dotdot_inside_missing_tail_denied) {
  /* ".." smuggled into a not-yet-existing suffix must not textually
   * cancel out (§6.4): rejected before spawn. */
  esc_fx f;
  astools_result r;
  astools_err e;
  memset(&r, 0, sizeof r);
  if (!esc_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    esc_drop(&f);
    return;
  }
  e = astools_invoke(f.c, "fk", "run",
                     "{ msg: \"x\", p: \"missing/../../../etc/pw\" }", 0,
                     &r);
  if (e == ASTOOLS_OK) {
    ASTOOLS_FAILF("dotdot-in-missing-tail was allowed: %s",
                r.result_xcdn ? r.result_xcdn : "");
    astools_result_free(&r);
    esc_drop(&f);
    return;
  }
  astools_result_free(&r);
  esc_drop(&f);
}

TEST(deadline_kill_leaves_engine_usable) {
  /* After a killed 5s sleeper, the very next invocation must work — no
   * orphan holds the slot, the scratch, or the stdio pumps. */
  esc_fx f;
  astools_result r;
  astools_err e;
  int64_t t0;
  memset(&r, 0, sizeof r);
  if (!esc_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    esc_drop(&f);
    return;
  }
  t0 = os_monotonic_ms();
  e = astools_invoke(f.c, "slp", "run", "{ ms: 5000 }", 250, &r);
  if (e != ASTOOLS_ERR_TIMEOUT) {
    ASTOOLS_FAILF("expected ERR_TIMEOUT, got %s", astools_err_name(e));
    astools_result_free(&r);
    esc_drop(&f);
    return;
  }
  astools_result_free(&r);
  if (os_monotonic_ms() - t0 >= 4000) {
    ASTOOLS_FAILF("deadline enforcement waited for the child");
    esc_drop(&f);
    return;
  }
  memset(&r, 0, sizeof r);
  if (astools_invoke(f.c, "fk", "run", "{ msg: \"alive\" }", 0, &r) !=
      ASTOOLS_OK) {
    ASTOOLS_FAILF("engine unusable after timeout: %s",
                astools_last_error(f.c));
    esc_drop(&f);
    return;
  }
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);
  esc_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(relative_traversal_denied_before_spawn),
  TEST_ENTRY(absolute_path_outside_workspace_denied),
#if !defined(_WIN32)
  TEST_ENTRY(symlink_inside_workspace_pointing_outside_denied),
#endif
  TEST_ENTRY(write_to_new_file_under_workspace_allowed),
  TEST_ENTRY(dotdot_inside_missing_tail_denied),
  TEST_ENTRY(deadline_kill_leaves_engine_usable),
};

RUN_ALL_TESTS()
