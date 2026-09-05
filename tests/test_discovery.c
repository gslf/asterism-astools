#include "astools_test.h"
#include "astools_internal.h"
#include "fakes.h"
typedef struct {
  char ws[256], root[256], cfg[512];
  astools_ctx *ctx;
} fixture;
static int setup(fixture *f) {
  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->ws) || !astools_test_tmpdir(f->root)) return 0;
  snprintf(f->cfg, sizeof f->cfg, "%s/config.xcdn", f->ws);
  if (!fake_registry_write(f->root, "aaa", NULL, NULL, NULL, NULL, NULL) ||
      !fake_registry_write(f->root, "zzz", NULL, NULL, NULL, NULL, NULL) ||
      !fake_config_write(f->cfg, f->root, 0, f->ws, NULL))
    return 0;
  astools_open_params p = {0};
  p.config_path = f->cfg;
  return astools_open(&p, &f->ctx) == ASTOOLS_OK;
}
static void drop(fixture *f) {
  astools_close(f->ctx);
  astools_test_rmtree(f->root);
  astools_test_rmtree(f->ws);
}
TEST(selection_is_consistent_and_revalidated) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  astools_selection *s = NULL, *next = NULL;
  astools_discovery_options o = {.intent = "zzz", .limit = 1};
  ASSERT_OK(astools_discover(f.ctx, &o, &s));
  ASSERT_EQ_INT(astools_selection_count(s), 1);
  ASSERT_EQ_INT(astools_selection_omitted(s), 1);
  const astools_selected_command *cmd = astools_selection_get(s, 0);
  ASSERT_TRUE(cmd && !strcmp(cmd->tool, "zzz.run") && !strcmp(cmd->ref, "zzz@1.0.0"));
  ASSERT_EQ_INT(strlen(cmd->content_sha256), 64);
  ASSERT_TRUE(strstr(astools_selection_schemas(s), "zzz.run") &&
              !strstr(astools_selection_schemas(s), "aaa.run"));
  ASSERT_TRUE(strstr(astools_selection_grammar(s), "zzz.run") &&
              !strstr(astools_selection_grammar(s), "aaa.run"));
  ASSERT_TRUE(strstr(astools_selection_catalog(s), "zzz.run") &&
              !strstr(astools_selection_catalog(s), "aaa.run"));
  uint64_t revision = astools_selection_revision(s);
  ASSERT_OK(astools_registry_refresh(f.ctx));
  ASSERT_OK(astools_discover(f.ctx, &o, &next));
  ASSERT_TRUE(astools_selection_revision(next) == revision);
  astools_selection_free(next);
  astools_result result;
  ASSERT_EQ_INT(astools_selection_invoke(s, "aaa.run", "{msg:\"no\"}", 100, &result),
                ASTOOLS_ERR_DENIED);
  ASSERT_OK(astools_selection_validate(s, "zzz.run", "{msg:\"no\"}"));
  ASSERT_EQ_INT(astools_selection_validate(s, "aaa.run", "{}"), ASTOOLS_ERR_DENIED);
  ASSERT_OK(astools_tool_enable(f.ctx, "zzz", 0));
  ASSERT_EQ_INT(astools_selection_validate(s, "zzz.run", "{msg:\"no\"}"), ASTOOLS_ERR_DENIED);
  ASSERT_EQ_INT(astools_selection_invoke(s, "zzz.run", "{msg:\"no\"}", 100, &result),
                ASTOOLS_ERR_DENIED);
  ASSERT_OK(astools_discover(f.ctx, &o, &next));
  ASSERT_TRUE(astools_selection_revision(next) > revision);
  ASSERT_TRUE(!strcmp(astools_selection_get(next, 0)->tool, "aaa.run"));
  astools_selection_free(next);
  ASSERT_OK(astools_tool_enable(f.ctx, "zzz", 1));
  char path[512];
  snprintf(path, sizeof path, "%s/zzz/manifest.xcdn", f.root);
  FILE *file = fopen(path, "ab");
  ASSERT_TRUE(file != NULL);
  ASSERT_EQ_INT(fputc('\n', file), '\n');
  ASSERT_EQ_INT(fclose(file), 0);
  /* Live bytes are checked even when no registry refresh has happened. */
  ASSERT_EQ_INT(astools_selection_validate(s, "zzz.run", "{msg:\"no\"}"), ASTOOLS_ERR_DENIED);
  ASSERT_EQ_INT(astools_selection_invoke(s, "zzz.run", "{msg:\"no\"}", 100, &result),
                ASTOOLS_ERR_DENIED);
  ASSERT_TRUE(strstr(astools_last_error(f.ctx), "changed after selection"));
  astools_selection_free(s);
  drop(&f);
}
TEST(discovery_budget_allowlist_and_static_denials) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  astools_selection *s = NULL;
  astools_discovery_options o = {.schema_budget = 1};
  ASSERT_OK(astools_discover(f.ctx, &o, &s));
  ASSERT_EQ_INT(astools_selection_count(s), 0);
  ASSERT_EQ_INT(astools_selection_omitted(s), 2);
  astools_selection_free(s);
  const char *allow[] = {"aaa.run"};
  o = (astools_discovery_options){.intent = "zzz", .allow = allow, .allow_count = 1};
  ASSERT_OK(astools_discover(f.ctx, &o, &s));
  ASSERT_EQ_INT(astools_selection_count(s), 1);
  ASSERT_TRUE(!strcmp(astools_selection_get(s, 0)->tool, "aaa.run"));
  astools_selection_free(s);
  ASSERT_TRUE(fake_registry_write(f.root, "network", NULL, NULL, "net: true, proc: false, fs: []",
                                  FAKE_CMD_RUN_EMPTY, NULL));
  ASSERT_TRUE(fake_registry_write(
      f.root, "write", NULL, NULL, NULL,
      "#command {name:\"run\",summary:\"write\",params:[#param "
      "{name:\"p\",required:true,type:#type {kind:\"path\",access:\"write\"}}]}",
      NULL));
  ASSERT_OK(astools_registry_refresh(f.ctx));
  f.ctx->cfg.workspace_access = ASTOOLS_ACCESS_READ;
  o = (astools_discovery_options){.intent = "network write", .limit = 64};
  ASSERT_OK(astools_discover(f.ctx, &o, &s));
  ASSERT_EQ_INT(astools_selection_count(s), 2);
  ASSERT_TRUE(!strstr(astools_selection_schemas(s), "network.run") &&
              !strstr(astools_selection_schemas(s), "write.run"));
  astools_selection_free(s);
  drop(&f);
}
TEST(late_relevant_tool_survives_large_registry) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  for (int i = 0; i < 80; i++) {
    char id[32];
    snprintf(id, sizeof id, "filler-%03d", i);
    ASSERT_TRUE(fake_registry_write(f.root, id, NULL, NULL, NULL, FAKE_CMD_RUN_EMPTY, NULL));
  }
  ASSERT_OK(astools_registry_refresh(f.ctx));
  astools_discovery_options o = {.intent = "zzz", .limit = 1};
  astools_selection *s = NULL;
  ASSERT_OK(astools_discover(f.ctx, &o, &s));
  ASSERT_EQ_INT(astools_selection_count(s), 1);
  ASSERT_EQ_INT(astools_selection_omitted(s), 81);
  ASSERT_TRUE(!strcmp(astools_selection_get(s, 0)->tool, "zzz.run"));
  astools_selection_free(s);
  drop(&f);
}
TEST_LIST = {TEST_ENTRY(late_relevant_tool_survives_large_registry),
             TEST_ENTRY(selection_is_consistent_and_revalidated),
             TEST_ENTRY(discovery_budget_allowlist_and_static_denials)};
RUN_ALL_TESTS()
