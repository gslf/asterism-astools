/* Semantic coding-tool integration tests against the packaged std binary. */

#include "astools_test.h"

#include "astools.h"

#ifndef ASTOOLS_STD_PACKAGES
#define ASTOOLS_STD_PACKAGES "packages"
#endif

static int write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  if (fputs(text, f) < 0) {
    fclose(f);
    return 0;
  }
  return fclose(f) == 0;
}

TEST(code_commands_and_proc_isolation) {
  char ws[256], ws_norm[256], source[512], nested[512];
  char config_path[512], config[2048];
  const char *roots[2] = {ASTOOLS_STD_PACKAGES, NULL};
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  char *after;
  size_t i;

  ASSERT_TRUE(astools_test_tmpdir(ws));
  snprintf(source, sizeof source, "%s/sample.c", ws);
  ASSERT_TRUE(write_text(source,
                         "int first = 1;\n"
                         "int target_symbol = 2;\n"
                         "int last = target_symbol;\n"));
  memset(&op, 0, sizeof op);
  op.registry_paths = roots;
  op.workspace_root = ws;
  ASSERT_OK(astools_open(&op, &c));

  /* A nested write is one operation: fs.write creates policy-confined
   * parents by default instead of returning fs/not-found. */
  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(c, "fs", "write",
                           "{path: \"nested/deep/file.txt\", "
                           "content: \"ok\\n\"}", 0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);
  snprintf(nested, sizeof nested, "%s/nested/deep/file.txt", ws);
  after = astools_test_read_file(nested, NULL);
  ASSERT_TRUE(after != NULL);
  ASSERT_EQ_STR(after, "ok\n");
  free(after);

  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(c, "code", "read-range",
                           "{path: \"sample.c\", start_line: 2, end_line: 3}",
                           0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(r.result_xcdn && strstr(r.result_xcdn, "target_symbol"));
  ASSERT_TRUE(strstr(r.result_xcdn, "total_lines") != NULL);
  astools_result_free(&r);

  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(c, "code", "search-symbol",
                           "{symbol: \"target_symbol\", path: \".\", "
                           "glob: \"*.c\"}",
                           0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(r.result_xcdn && strstr(r.result_xcdn, "sample.c"));
  astools_result_free(&r);

  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(
      c, "code", "apply-patch",
      "{patch: \"--- a/sample.c\\n+++ b/sample.c\\n@@ -1,3 +1,3 @@\\n"
      " int first = 1;\\n-int target_symbol = 2;\\n"
      "+int target_symbol = 3;\\n int last = target_symbol;\\n\"}",
      0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);
  after = astools_test_read_file(source, NULL);
  ASSERT_TRUE(after != NULL);
  ASSERT_TRUE(strstr(after, "target_symbol = 3") != NULL);
  free(after);

  /* A proc grant is scoped by tool id. With default grants, both the
   * semantic runner and the arbitrary runner are denied before spawn. */
  memset(&r, 0, sizeof r);
  ASSERT_ERR(astools_invoke(c, "project", "build", "{}", 0, &r),
             ASTOOLS_ERR_DENIED);
  astools_result_free(&r);
  memset(&r, 0, sizeof r);
  ASSERT_ERR(astools_invoke(c, "proc", "run", "{argv:[\"echo\",\"x\"]}",
                            0, &r),
             ASTOOLS_ERR_DENIED);
  astools_result_free(&r);

  astools_close(c);

  /* Grant project.proc only: the semantic wrapper reaches its adapter
   * detection, while proc.run remains denied. */
  snprintf(ws_norm, sizeof ws_norm, "%s", ws);
  for (i = 0; ws_norm[i]; i++)
    if (ws_norm[i] == '\\') ws_norm[i] = '/';
  snprintf(config_path, sizeof config_path, "%s/project-grant.xcdn", ws);
  snprintf(config, sizeof config,
           "#astools_config {"
           " registry:{paths:[{path:\"%s\",trust:\"standard\"}],"
           "watch:\"off\",pinning:\"off\"},"
           " workspace:{root:\"%s\"},"
           " grants:{workspace_access:\"read-write\","
           "tools:[{tool:\"project\",proc:true}]} }",
           ASTOOLS_STD_PACKAGES, ws_norm);
  ASSERT_TRUE(write_text(config_path, config));
  memset(&op, 0, sizeof op);
  op.config_path = config_path;
  ASSERT_OK(astools_open(&op, &c));
  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(c, "project", "build", "{}", 0, &r));
  ASSERT_EQ_INT(r.ok, 0);
  ASSERT_EQ_STR(r.error_code, "project/no-adapter");
  astools_result_free(&r);
  memset(&r, 0, sizeof r);
  ASSERT_ERR(astools_invoke(c, "proc", "run", "{argv:[\"echo\",\"x\"]}",
                            0, &r),
             ASTOOLS_ERR_DENIED);
  astools_result_free(&r);
  astools_close(c);
  astools_test_rmtree(ws);
}

TEST_LIST = {
  TEST_ENTRY(code_commands_and_proc_isolation),
};

RUN_ALL_TESTS()
