/*
 * test_invoke.c — the §5.1 pipeline end to end against the scripted
 * tool_fake binary (§17): validation, dispatch, protocol failures,
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

static int inv_setup(inv_fx *f) {
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

  snprintf(f->cfg_path, sizeof f->cfg_path, "%s/config.xcdn", f->cfg_raw);
  if (!fake_config_write(f->cfg_path, f->root_raw, 1, f->ws,
                         "invocation: { max_output_bytes: 4096,"
                         " result_validation: \"enforce\" },"))
    return 0;

  memset(&p, 0, sizeof p);
  p.config_path = f->cfg_path;
  return astools_open(&p, &f->c) == ASTOOLS_OK;
}

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
  /* the tool saw the canonical absolute path (§6.4) … */
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
  /* … and the injected default (D8) */
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
  /* contract: §5.1 step 1 — an unresolvable ref/command is NOT_FOUND */
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
  /* clean exit 0 with NO response is a protocol failure too (§5.2) */
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
   * (§5.6) */
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
   * echoed result violates the declared type => ERR_PROTOCOL (§5.1/7). */
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

TEST_LIST = {
  TEST_ENTRY(echo_canonical_path_and_default),
  TEST_ENTRY(validation_errors),
  TEST_ENTRY(crash_is_err_tool),
  TEST_ENTRY(bad_protocol_is_err_protocol),
  TEST_ENTRY(deadline_timeout_is_fast),
  TEST_ENTRY(flood_hits_output_cap),
  TEST_ENTRY(result_validation_enforce),
  TEST_ENTRY(sequential_invocations_reuse_context),
};

RUN_ALL_TESTS()
