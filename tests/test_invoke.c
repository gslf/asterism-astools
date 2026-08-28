/*
 * test_invoke.c — the pipeline end to end against the scripted
 * tool_fake binary: validation, dispatch, protocol failures,
 * deadlines, output caps, result validation, stats.
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "fakes.h"
#include "xcdn.h"

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

/* Fixture: full-trust root with every behavior registered as its own
 * tool, a tmp workspace, and one context. Config: max_output_bytes 4096
 * (flood cap), result_validation enforce (exercised by tool "ret"). */
typedef struct {
  char ws_raw[256], root_raw[256], cfg_raw[256];
  char cfg_path[512];
  char *ws; /* canonical */
  astools_ctx *c;
} inv_fx;

static int inv_setup_level(inv_fx *f, const char *sandbox_extra) {
  astools_open_params p;
  static const char RET_CMDS[] =
      "#command {\n"
      "  name: \"run\",\n"
      "  summary: \"Echo with an impossible declared return type.\",\n"
      "  params: [\n"
      "    #param { name: \"msg\", type: #type { kind: \"string\" },"
      " required: true },\n"
      "  ],\n"
      "  returns: { type: #type { kind: \"object\", fields: [\n"
      "    #param { name: \"impossible\", type: #type {"
      " kind: \"string\" }, required: true },\n"
      "  ] }, description: \"Never satisfied by the echo result.\" },\n"
      "  examples: [ #example { call: { msg: \"x\" },"
      " result: { impossible: \"y\" } } ],\n"
      "}";

  memset(f, 0, sizeof *f);
  if (tool_path()[0] == '\0') return 0;
  if (!astools_test_tmpdir(f->ws_raw) || !astools_test_tmpdir(f->root_raw) ||
      !astools_test_tmpdir(f->cfg_raw))
    return 0;
  if (os_realpath(f->ws_raw, &f->ws) != ASTOOLS_OK) return 0;

  if (!fake_registry_write(f->root_raw, "fk", tool_path(), "echo", NULL,
                           NULL, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "slp", tool_path(), "sleep", NULL,
                           FAKE_CMD_RUN_MS, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "crsh", tool_path(), "crash", NULL,
                           FAKE_CMD_RUN_EMPTY, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "bad", tool_path(), "badproto",
                           NULL, FAKE_CMD_RUN_EMPTY, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "nores", tool_path(), "noresponse",
                           NULL, FAKE_CMD_RUN_EMPTY, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "fld", tool_path(), "flood", NULL,
                           FAKE_CMD_RUN_EMPTY, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "ret", tool_path(), "echo", NULL,
                           RET_CMDS, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "netp", tool_path(), "netprobe",
                           NULL, FAKE_CMD_RUN_EMPTY, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "pathp", tool_path(), "pathprobe",
                           NULL, FAKE_CMD_RUN_EMPTY, NULL))
    return 0;

  snprintf(f->cfg_path, sizeof f->cfg_path, "%s/config.xcdn", f->cfg_raw);
  {
    char extra[1024];
    snprintf(extra, sizeof extra,
             "invocation: { max_output_bytes: 4096,"
             " result_validation: \"enforce\" },%s",
             sandbox_extra ? sandbox_extra : "");
    if (!fake_config_write(f->cfg_path, f->root_raw, 1, f->ws, extra))
      return 0;
  }

  memset(&p, 0, sizeof p);
  p.config_path = f->cfg_path;
  return astools_open(&p, &f->c) == ASTOOLS_OK;
}

static int inv_setup(inv_fx *f) { return inv_setup_level(f, NULL); }

static void inv_drop(inv_fx *f) {
  if (f->c) astools_close(f->c);
  free(f->ws);
  astools_test_rmtree(f->ws_raw);
  astools_test_rmtree(f->root_raw);
  astools_test_rmtree(f->cfg_raw);
}

/* String field of a parsed result object; NULL when absent/mistyped. */
static char *result_str(const char *result_xcdn, const char *key) {
  xcdn_error_t xe;
  xcdn_document_t *doc;
  char *out = NULL;
  memset(&xe, 0, sizeof xe);
  doc = xcdn_parse(result_xcdn, &xe);
  if (!doc) return NULL;
  if (doc->values_len > 0 && doc->values[0]->value &&
      doc->values[0]->value->type == XCDN_VAL_OBJECT) {
    const xcdn_node_t *n = xcdn_object_get(doc->values[0]->value, key);
    if (n && n->value && n->value->type == XCDN_VAL_STRING &&
        n->value->data.string)
      out = astools_strdup(n->value->data.string);
  }
  xcdn_document_free(doc);
  return out;
}

static int result_int(const char *result_xcdn, const char *key,
                      int64_t *out) {
  xcdn_error_t xe;
  xcdn_document_t *doc;
  int found = 0;
  memset(&xe, 0, sizeof xe);
  doc = xcdn_parse(result_xcdn, &xe);
  if (!doc) return 0;
  if (doc->values_len > 0 && doc->values[0]->value &&
      doc->values[0]->value->type == XCDN_VAL_OBJECT) {
    const xcdn_node_t *n = xcdn_object_get(doc->values[0]->value, key);
    if (n && n->value && n->value->type == XCDN_VAL_INT) {
      *out = n->value->data.integer;
      found = 1;
    }
  }
  xcdn_document_free(doc);
  return found;
}

static int result_bool(const char *result_xcdn, const char *key, int *out) {
  xcdn_error_t xe;
  xcdn_document_t *doc;
  int found = 0;
  memset(&xe, 0, sizeof xe);
  doc = xcdn_parse(result_xcdn, &xe);
  if (!doc) return 0;
  if (doc->values_len > 0 && doc->values[0]->value &&
      doc->values[0]->value->type == XCDN_VAL_OBJECT) {
    const xcdn_node_t *n = xcdn_object_get(doc->values[0]->value, key);
    if (n && n->value && n->value->type == XCDN_VAL_BOOL) {
      *out = n->value->data.boolean ? 1 : 0;
      found = 1;
    }
  }
  xcdn_document_free(doc);
  return found;
}

TEST(echo_canonical_path_and_default) {
  inv_fx f;
  astools_result r;
  astools_stats before, after;
  char want[1024];
  char *p_val;
  int64_t count = 0;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed (is ASTOOLS_TEST_TOOL_PATH set?)");
    inv_drop(&f);
    return;
  }
  ASSERT_OK(astools_get_stats(f.c, &before));
  if (astools_invoke(f.c, "fk", "run",
                     "{ msg: \"hi\", p: \"data/out.txt\" }", 0,
                     &r) != ASTOOLS_OK) {
    ASTOOLS_FAILF("invoke failed: %s", astools_last_error(f.c));
    inv_drop(&f);
    return;
  }
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(r.result_xcdn != NULL);
  /* the tool saw the canonical absolute path … */
  snprintf(want, sizeof want, "%s/data/out.txt", f.ws);
  p_val = result_str(r.result_xcdn, "p");
  if (p_val == NULL || strcmp(p_val, want) != 0) {
    ASTOOLS_FAILF("echoed p = '%s', want '%s' (result: %s)",
                p_val ? p_val : "(null)", want, r.result_xcdn);
    free(p_val);
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  free(p_val);
  /* … and the injected default */
  if (!result_int(r.result_xcdn, "count", &count) || count != 7) {
    ASTOOLS_FAILF("default count=7 not injected (result: %s)",
                r.result_xcdn);
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  astools_result_free(&r);
  ASSERT_OK(astools_get_stats(f.c, &after));
  ASSERT_TRUE(after.invocations > before.invocations);
  ASSERT_TRUE(after.ok > before.ok);
  ASSERT_TRUE(after.last_invocation_unix > 0);
  inv_drop(&f);
}

TEST(validation_errors) {
  inv_fx f;
  astools_result r;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  /* unknown key */
  ASSERT_ERR(astools_invoke(f.c, "fk", "run",
                          "{ msg: \"hi\", nope: 1 }", 0, &r),
             ASTOOLS_ERR_INVALID);
  astools_result_free(&r);
  /* wrong type (strict, no coercion) */
  ASSERT_ERR(astools_invoke(f.c, "fk", "run", "{ msg: 3 }", 0, &r),
             ASTOOLS_ERR_INVALID);
  astools_result_free(&r);
  /* missing required */
  ASSERT_ERR(astools_invoke(f.c, "fk", "run", "{}", 0, &r),
             ASTOOLS_ERR_INVALID);
  astools_result_free(&r);
  /* contract: step 1 — an unresolvable ref/command is NOT_FOUND */
  ASSERT_ERR(astools_invoke(f.c, "fk", "nope", "{ msg: \"x\" }", 0, &r),
             ASTOOLS_ERR_NOT_FOUND);
  astools_result_free(&r);
  ASSERT_ERR(astools_invoke(f.c, "ghost", "run", "{}", 0, &r),
             ASTOOLS_ERR_NOT_FOUND);
  astools_result_free(&r);
  /* validate-only path agrees */
  ASSERT_OK(astools_validate_args(f.c, "fk", "run", "{ msg: \"x\" }"));
  ASSERT_ERR(astools_validate_args(f.c, "fk", "run", "{ msg: 3 }"),
             ASTOOLS_ERR_INVALID);
  inv_drop(&f);
}

TEST(crash_is_err_tool) {
  inv_fx f;
  astools_result r;
  astools_stats before, after;
  astools_err e;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  ASSERT_OK(astools_get_stats(f.c, &before));
  e = astools_invoke(f.c, "crsh", "run", "{}", 0, &r);
  if (e != ASTOOLS_ERR_TOOL) {
    ASTOOLS_FAILF("expected ERR_TOOL, got %s (%s)", astools_err_name(e),
                astools_last_error(f.c));
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  ASSERT_EQ_INT(r.ok, 0);
  ASSERT_EQ_STR(r.error_code, "astools/tool-crashed");
  astools_result_free(&r);
  ASSERT_OK(astools_get_stats(f.c, &after));
  ASSERT_TRUE(after.failed > before.failed);
  inv_drop(&f);
}

TEST(bad_protocol_is_err_protocol) {
  inv_fx f;
  astools_result r;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  ASSERT_ERR(astools_invoke(f.c, "bad", "run", "{}", 0, &r),
             ASTOOLS_ERR_PROTOCOL);
  astools_result_free(&r);
  /* clean exit 0 with NO response is a protocol failure too */
  ASSERT_ERR(astools_invoke(f.c, "nores", "run", "{}", 0, &r),
             ASTOOLS_ERR_PROTOCOL);
  astools_result_free(&r);
  inv_drop(&f);
}

TEST(deadline_timeout_is_fast) {
  inv_fx f;
  astools_result r;
  astools_stats before, after;
  int64_t t0, elapsed;
  astools_err e;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  ASSERT_OK(astools_get_stats(f.c, &before));
  t0 = os_monotonic_ms();
  e = astools_invoke(f.c, "slp", "run", "{ ms: 5000 }", 300, &r);
  elapsed = os_monotonic_ms() - t0;
  if (e != ASTOOLS_ERR_TIMEOUT) {
    ASTOOLS_FAILF("expected ERR_TIMEOUT, got %s", astools_err_name(e));
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  astools_result_free(&r);
  if (elapsed >= 3000)
    ASTOOLS_FAILF("timeout took %lld ms (deadline was 300ms)",
                (long long)elapsed);
  ASSERT_OK(astools_get_stats(f.c, &after));
  ASSERT_TRUE(after.timeouts > before.timeouts);
  inv_drop(&f);
}

TEST(flood_hits_output_cap) {
  inv_fx f;
  astools_result r;
  astools_err e;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  /* invocation.max_output_bytes = 4096; overflow kills, never truncates
   * */
  e = astools_invoke(f.c, "fld", "run", "{}", 10000, &r);
  if (e != ASTOOLS_ERR_TOOL) {
    ASTOOLS_FAILF("expected ERR_TOOL for overflow, got %s",
                astools_err_name(e));
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  ASSERT_EQ_STR(r.error_code, "astools/overflow");
  astools_result_free(&r);
  inv_drop(&f);
}

TEST(result_validation_enforce) {
  /* invocation.result_validation = enforce: a returns-typed command whose
   * echoed result violates the declared type => ERR_PROTOCOL (7). */
  inv_fx f;
  astools_result r;
  astools_err e;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  e = astools_invoke(f.c, "ret", "run", "{ msg: \"hi\" }", 0, &r);
  if (e != ASTOOLS_ERR_PROTOCOL) {
    ASTOOLS_FAILF("expected ERR_PROTOCOL under enforce, got %s",
                astools_err_name(e));
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  astools_result_free(&r);
  /* the untyped echo tool is unaffected (nothing declared, nothing to
   * mismatch) */
  ASSERT_OK(astools_invoke(f.c, "fk", "run", "{ msg: \"ok\" }", 0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);
  inv_drop(&f);
}

TEST(sequential_invocations_reuse_context) {
  inv_fx f;
  astools_result r;
  int i;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  for (i = 0; i < 3; i++) {
    if (astools_invoke(f.c, "fk", "run", "{ msg: \"again\" }", 0, &r) !=
        ASTOOLS_OK) {
      ASTOOLS_FAILF("iteration %d failed: %s", i, astools_last_error(f.c));
      astools_result_free(&r);
      inv_drop(&f);
      return;
    }
    ASSERT_EQ_INT(r.ok, 1);
    astools_result_free(&r);
  }
  inv_drop(&f);
}

TEST(async_cancel_is_race_free_and_fast) {
  inv_fx f;
  astools_task *task = NULL;
  astools_result r;
  int64_t start;
  memset(&r, 0, sizeof r);
  if (!inv_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    inv_drop(&f);
    return;
  }
  start = os_monotonic_ms();
  {
    astools_err e = astools_invoke_async(f.c, "slp", "run",
                                         "{ ms: 3000 }", 5000, &task);
    if (e == ASTOOLS_ERR_UNSUPPORTED) {
      inv_drop(&f); /* expected for ASTOOLS_NO_THREADS builds */
      return;
    }
    ASSERT_OK(e);
  }
  ASSERT_ERR(astools_task_wait(task, 1, NULL), ASTOOLS_ERR_BUSY);
  ASSERT_OK(astools_task_cancel(task));
  ASSERT_ERR(astools_task_wait(task, 2000, &r), ASTOOLS_ERR_CANCELLED);
  ASSERT_TRUE(os_monotonic_ms() - start < 2000);
  ASSERT_EQ_STR(r.error_code, "astools/cancelled");
  astools_result_free(&r);
  astools_task_free(task);
  inv_drop(&f);
}

TEST(strict_linux_executes_when_available) {
  inv_fx f;
  astools_sandbox_caps caps;
  astools_result r;
  int socket_ok = 1;
  memset(&r, 0, sizeof r);
  if (!inv_setup_level(
          &f,
          "sandbox: { default_level: \"strict\", strict_fallback: \"reject\" },")) {
    ASTOOLS_FAILF("strict setup failed");
    inv_drop(&f);
    return;
  }
  ASSERT_OK(astools_get_sandbox_caps(f.c, 1, &caps));
  if (!caps.fs_confinement) {
    inv_drop(&f); /* old kernel/platform: capability report is the contract */
    return;
  }
  ASSERT_TRUE(caps.net_deny && caps.privilege_drop);
#if defined(__linux__)
  /* seccomp. The macOS Seatbelt profile confines the filesystem and denies
   * the network, but has no syscall-filter equivalent, so the capability
   * report leaves this bit zero there (sandbox_posix.c). */
  ASSERT_TRUE(caps.syscall_filter);
#endif
  if (astools_invoke(f.c, "fk", "run", "{ msg: \"strict\" }", 2000, &r) !=
      ASTOOLS_OK) {
    ASTOOLS_FAILF("strict invoke failed: %s", astools_last_error(f.c));
    astools_result_free(&r);
    inv_drop(&f);
    return;
  }
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);
  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(f.c, "netp", "run", "{}", 2000, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(result_bool(r.result_xcdn, "socket_ok", &socket_ok));
  /* The probe only calls socket(2). That is refused by the seccomp filter,
   * so what it really observes is syscall_filter, not net_deny: the macOS
   * Seatbelt profile denies network *operations* (bind/connect) and lets
   * the socket itself be created. Tie the expectation to the capability
   * that produces it rather than to the platform. */
  if (caps.syscall_filter) ASSERT_EQ_INT(socket_ok, 0);
  astools_result_free(&r);
  inv_drop(&f);
}

TEST(configured_executable_paths_reach_child) {
  inv_fx f;
  astools_result r;
  int found = 0;
  memset(&r, 0, sizeof r);
#if defined(_WIN32)
  if (!inv_setup_level(
          &f,
          "sandbox: { executable_paths: [\"C:/astools-explicit-bin\"] },")) {
#else
  if (!inv_setup_level(
          &f,
          "sandbox: { executable_paths: [\"/astools-explicit-bin\"] },")) {
#endif
    ASTOOLS_FAILF("executable-path setup failed");
    inv_drop(&f);
    return;
  }
  ASSERT_EQ_INT(f.c->cfg.executable_paths_len, 1);
  ASSERT_OK(astools_invoke(f.c, "pathp", "run", "{}", 2000, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(result_bool(r.result_xcdn, "found", &found));
  ASSERT_EQ_INT(found, 1);
  astools_result_free(&r);
  inv_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(echo_canonical_path_and_default),
  TEST_ENTRY(validation_errors),
  TEST_ENTRY(crash_is_err_tool),
  TEST_ENTRY(bad_protocol_is_err_protocol),
  TEST_ENTRY(deadline_timeout_is_fast),
  TEST_ENTRY(flood_hits_output_cap),
  TEST_ENTRY(result_validation_enforce),
  TEST_ENTRY(sequential_invocations_reuse_context),
  TEST_ENTRY(async_cancel_is_race_free_and_fast),
  TEST_ENTRY(strict_linux_executes_when_available),
  TEST_ENTRY(configured_executable_paths_reach_child),
};

RUN_ALL_TESTS()
