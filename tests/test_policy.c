/*
 * test_policy.c — policy.c: grants intersection + invocation pre-flight
 * (§6.3–§6.4, §17). Runs over a real context (astools_open on a tmp
 * workspace + registry) so the effective policy is computed exactly as in
 * production; nothing is ever spawned (dummy package-relative binaries).
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "fakes.h"
#include "xcdn.h"

/* Fixture: workspace + registry root with three tools:
 *   fk   default echo command (msg, p path rw, count default)
 *   wide extra fs request on `outside`, q path read must_exist
 *   pr   requests proc (never granted)
 * plus a config granting `wide` read on `outside`. */
typedef struct {
  char ws_raw[256], root_raw[256], out_raw[256], cfgdir_raw[256];
  char cfg_path[512];
  char *ws, *outside;
  astools_ctx *c;
} pol_fx;

static int pol_setup(pol_fx *f) {
  astools_open_params p;
  astools_buf extra;
  char perms[1024], cmds[1024], target[512];
  FILE *fp;
  int ok = 0;

  memset(f, 0, sizeof *f);
  astools_buf_init(&extra);
  if (!astools_test_tmpdir(f->ws_raw) || !astools_test_tmpdir(f->root_raw) ||
      !astools_test_tmpdir(f->out_raw) || !astools_test_tmpdir(f->cfgdir_raw))
    return 0;
  if (os_realpath(f->ws_raw, &f->ws) != ASTOOLS_OK) return 0;
  if (os_realpath(f->out_raw, &f->outside) != ASTOOLS_OK) return 0;

  snprintf(target, sizeof target, "%s/f.txt", f->outside);
  fp = fopen(target, "wb");
  if (!fp) return 0;
  fputs("data", fp);
  fclose(fp);

  if (!fake_registry_write(f->root_raw, "fk", NULL, NULL, NULL, NULL, NULL))
    return 0;

  snprintf(perms, sizeof perms,
           "fs: [ { path: \"${workspace}\", access: \"read-write\" },"
           " { path: \"%s\", access: \"read\" } ],\n"
           "    net: false, proc: false, env: []",
           f->outside);
  snprintf(cmds, sizeof cmds,
           "#command {\n"
           "  name: \"run\",\n"
           "  summary: \"Read a granted outside file.\",\n"
           "  params: [\n"
           "    #param { name: \"q\", type: #type { kind: \"path\","
           " access: \"read\", must_exist: true }, required: true },\n"
           "  ],\n"
           "  examples: [ #example { call: { q: \"f.txt\" },"
           " result: {} } ],\n"
           "}");
  if (!fake_registry_write(f->root_raw, "wide", NULL, NULL, perms, cmds,
                           NULL))
    return 0;

  if (!fake_registry_write(f->root_raw, "pr", NULL, NULL,
                           "fs: [ { path: \"${workspace}\","
                           " access: \"read-write\" } ],\n"
                           "    net: false, proc: true, env: []",
                           FAKE_CMD_RUN_EMPTY, NULL))
    return 0;

  snprintf(f->cfg_path, sizeof f->cfg_path, "%s/config.xcdn",
           f->cfgdir_raw);
  if (astools_buf_printf(&extra,
                         "grants: {\n"
                         "    workspace_access: \"read-write\",\n"
                         "    tools: [ { tool: \"wide\", fs: ["
                         " { path: \"%s\", access: \"read\" } ] } ],\n"
                         "  },",
                         f->outside) != ASTOOLS_OK) {
    astools_buf_free(&extra);
    return 0;
  }
  ok = fake_config_write(f->cfg_path, f->root_raw, 0, f->ws, extra.data);
  astools_buf_free(&extra);
  if (!ok) return 0;

  memset(&p, 0, sizeof p);
  p.config_path = f->cfg_path;
  if (astools_open(&p, &f->c) != ASTOOLS_OK) return 0;
  return 1;
}

static void pol_drop(pol_fx *f) {
  if (f->c) astools_close(f->c);
  free(f->ws);
  free(f->outside);
  astools_test_rmtree(f->ws_raw);
  astools_test_rmtree(f->root_raw);
  astools_test_rmtree(f->out_raw);
  astools_test_rmtree(f->cfgdir_raw);
}

static xcdn_node_t *parse_args(xcdn_document_t **doc, const char *src) {
  xcdn_error_t xe;
  memset(&xe, 0, sizeof xe);
  *doc = xcdn_parse(src, &xe);
  if (!*doc || (*doc)->values_len == 0) return NULL;
  return (*doc)->values[0];
}

static const char *obj_string(const xcdn_node_t *args, const char *key) {
  const xcdn_node_t *n;
  if (!args || !args->value) return NULL;
  n = xcdn_object_get(args->value, key);
  if (!n || !n->value || n->value->type != XCDN_VAL_STRING) return NULL;
  return n->value->data.string;
}

TEST(workspace_auto_grant_intersection) {
  pol_fx f;
  astools_tool *t = NULL;
  astools_effective eff;
  size_t i;
  int found = 0;
  if (!pol_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    pol_drop(&f);
    return;
  }
  memset(&eff, 0, sizeof eff);
  if (astools_registry_resolve(f.c, "fk", &t) != ASTOOLS_OK) {
    ASTOOLS_FAILF("resolve fk failed: %s", astools_last_error(f.c));
    pol_drop(&f);
    return;
  }
  if (astools_policy_effective(f.c, t, &eff) != ASTOOLS_OK) {
    ASTOOLS_FAILF("policy_effective failed");
    astools_tool_unref(t);
    pol_drop(&f);
    return;
  }
  /* manifest ${workspace} rw ∩ workspace auto-grant rw => workspace rw */
  for (i = 0; i < eff.fs_len; i++) {
    if (eff.fs[i].path && strcmp(eff.fs[i].path, f.ws) == 0 &&
        eff.fs[i].access == ASTOOLS_ACCESS_RW)
      found = 1;
  }
  if (!found) {
    ASTOOLS_FAILF("effective fs does not contain workspace rw (n=%zu)",
                eff.fs_len);
  }
  if (eff.net || eff.proc)
    ASTOOLS_FAILF("net/proc granted without a host grant");
  astools_effective_free(&eff);
  astools_tool_unref(t);
  pol_drop(&f);
}

TEST(preflight_replaces_path_with_canonical) {
  pol_fx f;
  astools_tool *t = NULL;
  astools_effective eff;
  xcdn_document_t *doc = NULL;
  xcdn_node_t *args;
  char *deny = NULL;
  char want[1024];
  const astools_cmd *cmd;
  if (!pol_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    pol_drop(&f);
    return;
  }
  memset(&eff, 0, sizeof eff);
  if (astools_registry_resolve(f.c, "fk", &t) != ASTOOLS_OK ||
      astools_policy_effective(f.c, t, &eff) != ASTOOLS_OK) {
    ASTOOLS_FAILF("resolve/effective failed");
    if (t) astools_tool_unref(t);
    pol_drop(&f);
    return;
  }
  cmd = astools_manifest_cmd(t->m, "run");
  args = parse_args(&doc, "{ msg: \"hi\", p: \"sub/data.txt\" }");
  if (!cmd || !args) {
    ASTOOLS_FAILF("fixture parse failed");
    goto out;
  }
  if (astools_policy_preflight(f.c, t, cmd, args, &eff, &deny) !=
      ASTOOLS_OK) {
    ASTOOLS_FAILF("preflight denied: %s", deny ? deny : "(no message)");
    goto out;
  }
  /* the canonical absolute path REPLACED the original inside args (§6.4) */
  snprintf(want, sizeof want, "%s/sub/data.txt", f.ws);
  if (obj_string(args, "p") == NULL ||
      strcmp(obj_string(args, "p"), want) != 0) {
    ASTOOLS_FAILF("args.p = '%s', want '%s'",
                obj_string(args, "p") ? obj_string(args, "p") : "(null)",
                want);
  }
out:
  free(deny);
  if (doc) xcdn_document_free(doc);
  astools_effective_free(&eff);
  astools_tool_unref(t);
  pol_drop(&f);
}

TEST(preflight_denies_outside_grants) {
  pol_fx f;
  astools_tool *t = NULL;
  astools_effective eff;
  xcdn_document_t *doc = NULL;
  xcdn_node_t *args;
  char *deny = NULL;
  const astools_cmd *cmd;
  astools_err e;
  if (!pol_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    pol_drop(&f);
    return;
  }
  memset(&eff, 0, sizeof eff);
  if (astools_registry_resolve(f.c, "fk", &t) != ASTOOLS_OK ||
      astools_policy_effective(f.c, t, &eff) != ASTOOLS_OK) {
    ASTOOLS_FAILF("resolve/effective failed");
    if (t) astools_tool_unref(t);
    pol_drop(&f);
    return;
  }
  cmd = astools_manifest_cmd(t->m, "run");
  args = parse_args(&doc, "{ msg: \"hi\", p: \"../escape.txt\" }");
  if (!cmd || !args) {
    ASTOOLS_FAILF("fixture parse failed");
    goto out;
  }
  e = astools_policy_preflight(f.c, t, cmd, args, &eff, &deny);
  if (e != ASTOOLS_ERR_DENIED) {
    ASTOOLS_FAILF("expected DENIED, got %s", astools_err_name(e));
    goto out;
  }
  /* actionable message: names the missing grant (§6.3) */
  if (deny == NULL || strstr(deny, "grant") == NULL) {
    ASTOOLS_FAILF("deny message not actionable: %s",
                deny ? deny : "(null)");
  }
out:
  free(deny);
  if (doc) xcdn_document_free(doc);
  astools_effective_free(&eff);
  astools_tool_unref(t);
  pol_drop(&f);
}

TEST(preflight_denies_ungranted_proc) {
  pol_fx f;
  astools_tool *t = NULL;
  astools_effective eff;
  xcdn_document_t *doc = NULL;
  xcdn_node_t *args;
  char *deny = NULL;
  const astools_cmd *cmd;
  astools_err e;
  if (!pol_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    pol_drop(&f);
    return;
  }
  memset(&eff, 0, sizeof eff);
  if (astools_registry_resolve(f.c, "pr", &t) != ASTOOLS_OK ||
      astools_policy_effective(f.c, t, &eff) != ASTOOLS_OK) {
    ASTOOLS_FAILF("resolve/effective failed");
    if (t) astools_tool_unref(t);
    pol_drop(&f);
    return;
  }
  ASSERT_TRUE(!eff.proc); /* requested but never granted */
  cmd = astools_manifest_cmd(t->m, "run");
  args = parse_args(&doc, "{}");
  if (!cmd || !args) {
    ASTOOLS_FAILF("fixture parse failed");
    goto out;
  }
  e = astools_policy_preflight(f.c, t, cmd, args, &eff, &deny);
  if (e != ASTOOLS_ERR_DENIED) {
    ASTOOLS_FAILF("expected DENIED for ungranted proc, got %s",
                astools_err_name(e));
    goto out;
  }
  if (deny == NULL || strstr(deny, "proc") == NULL)
    ASTOOLS_FAILF("deny message does not name proc: %s",
                deny ? deny : "(null)");
out:
  free(deny);
  if (doc) xcdn_document_free(doc);
  astools_effective_free(&eff);
  astools_tool_unref(t);
  pol_drop(&f);
}

TEST(per_tool_grant_widening) {
  pol_fx f;
  astools_tool *t = NULL;
  astools_effective eff;
  xcdn_document_t *doc = NULL;
  xcdn_node_t *args;
  char *deny = NULL;
  char q[1024], want[1024];
  size_t i;
  int found = 0;
  const astools_cmd *cmd;
  if (!pol_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    pol_drop(&f);
    return;
  }
  memset(&eff, 0, sizeof eff);
  if (astools_registry_resolve(f.c, "wide", &t) != ASTOOLS_OK ||
      astools_policy_effective(f.c, t, &eff) != ASTOOLS_OK) {
    ASTOOLS_FAILF("resolve/effective failed");
    if (t) astools_tool_unref(t);
    pol_drop(&f);
    return;
  }
  /* request outside:read ∩ grants.tools outside:read => outside:read */
  for (i = 0; i < eff.fs_len; i++) {
    if (eff.fs[i].path && strcmp(eff.fs[i].path, f.outside) == 0 &&
        (eff.fs[i].access & ASTOOLS_ACCESS_READ) != 0)
      found = 1;
  }
  if (!found) {
    ASTOOLS_FAILF("outside prefix missing from effective set (n=%zu)",
                eff.fs_len);
    goto out;
  }
  cmd = astools_manifest_cmd(t->m, "run");
  snprintf(q, sizeof q, "{ q: \"%s/f.txt\" }", f.outside);
  args = parse_args(&doc, q);
  if (!cmd || !args) {
    ASTOOLS_FAILF("fixture parse failed");
    goto out;
  }
  if (astools_policy_preflight(f.c, t, cmd, args, &eff, &deny) !=
      ASTOOLS_OK) {
    ASTOOLS_FAILF("widened path denied: %s", deny ? deny : "(no message)");
    goto out;
  }
  snprintf(want, sizeof want, "%s/f.txt", f.outside);
  if (obj_string(args, "q") == NULL ||
      strcmp(obj_string(args, "q"), want) != 0)
    ASTOOLS_FAILF("args.q = '%s', want '%s'",
                obj_string(args, "q") ? obj_string(args, "q") : "(null)",
                want);
out:
  free(deny);
  if (doc) xcdn_document_free(doc);
  astools_effective_free(&eff);
  astools_tool_unref(t);
  pol_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(workspace_auto_grant_intersection),
  TEST_ENTRY(preflight_replaces_path_with_canonical),
  TEST_ENTRY(preflight_denies_outside_grants),
  TEST_ENTRY(preflight_denies_ungranted_proc),
  TEST_ENTRY(per_tool_grant_widening),
};

RUN_ALL_TESTS()
