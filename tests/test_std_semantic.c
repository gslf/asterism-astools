/* Semantic coding-tool integration tests against the packaged std binary. */

#include "astools_test.h"

#include "astools.h"
#include "xcdn.h"

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

TEST(edit_patch_relocates_unique_stale_hunk) {
  char ws[256], source[512];
  const char *roots[2] = {ASTOOLS_STD_PACKAGES, NULL};
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  char *after;

  ASSERT_TRUE(astools_test_tmpdir(ws));
  snprintf(source, sizeof source, "%s/shifted.c", ws);
  ASSERT_TRUE(write_text(source,
                         "/* lines inserted after the diff was made */\n"
                         "#define FEATURE 1\n"
                         "static int terminal_initialized = 0;\n"
                         "\n"
                         "static void die(void) {}\n"));
  memset(&op, 0, sizeof op);
  op.registry_paths = roots;
  op.workspace_root = ws;
  ASSERT_OK(astools_open(&op, &c));

  memset(&r, 0, sizeof r);
  ASSERT_OK(astools_invoke(
      c, "edit", "patch",
      "{patch: \"--- a/shifted.c\\n+++ b/shifted.c\\n@@ -1,3 +1,5 @@\\n"
      " static int terminal_initialized = 0;\\n \\n"
      "+static void restore_terminal(void);\\n+\\n"
      " static void die(void) {}\\n\"}",
      0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);
  after = astools_test_read_file(source, NULL);
  ASSERT_TRUE(after != NULL);
  ASSERT_TRUE(strstr(after, "static void restore_terminal(void);\n\n"
                            "static void die(void) {}") != NULL);
  free(after);
  astools_close(c);
  astools_test_rmtree(ws);
}

TEST(edits_reject_stale_versions) {
  char ws[256], path[512], hash[65], args[1024];
  const char *roots[] = {ASTOOLS_STD_PACKAGES, NULL};
  astools_open_params op = {0};
  astools_ctx *c = NULL;
  astools_result r = {0};
  xcdn_document_t *doc;
  const xcdn_node_t *node;
  ASSERT_TRUE(astools_test_tmpdir(ws));
  snprintf(path, sizeof path, "%s/target.c", ws);
  ASSERT_TRUE(write_text(path, "int x = 1;\n"));
  op.registry_paths = roots; op.workspace_root = ws;
  ASSERT_OK(astools_open(&op, &c));
  ASSERT_OK(astools_invoke(c, "code", "read-range", "{path:\"target.c\"}", 0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  doc = xcdn_parse(r.result_xcdn, NULL);
  ASSERT_TRUE(doc && doc->values_len == 1);
  node = xcdn_object_get(doc->values[0]->value, "sha256");
  ASSERT_TRUE(node && node->value->type == XCDN_VAL_STRING);
  snprintf(hash, sizeof hash, "%s", node->value->data.string);
  xcdn_document_free(doc); astools_result_free(&r);
  /* The target text still occurs exactly once, but its dependency changed. */
  ASSERT_TRUE(write_text(path, "// external edit\nint x = 1;\n"));
  snprintf(args, sizeof args, "{path:\"target.c\", find:\"int x = 1;\","
           "replace_with:\"int x = 2;\", expected_sha256:\"%s\"}", hash);
  ASSERT_OK(astools_invoke(c, "edit", "replace", args, 0, &r));
  ASSERT_EQ_INT(r.ok, 0);
  ASSERT_EQ_STR(r.error_code, "edit/conflict");
  astools_result_free(&r);
  ASSERT_TRUE(write_text(path, "int x = 1;\n"));
  snprintf(args, sizeof args, "{expected:[{path:\"target.c\",sha256:\"%s\"}],"
      "patch:\"--- a/target.c\\n+++ b/target.c\\n@@ -1 +1 @@\\n-int x = 1;\\n+int x = 2;\\n\"}", hash);
  /* The patch string above is xCDN-escaped, as a caller would send it. */
  ASSERT_OK(astools_invoke(c, "code", "apply-patch", args, 0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(strstr(r.result_xcdn, "after_sha256") != NULL);
  astools_result_free(&r);
  ASSERT_OK(astools_invoke(c, "code", "apply-patch", args, 0, &r));
  ASSERT_EQ_INT(r.ok, 0);
  astools_result_free(&r);
  astools_close(c); astools_test_rmtree(ws);
}

TEST(project_test_reports_real_collection) {
  char ws[256], path[512], config_path[512], config[2048];
  astools_open_params op = {0};
  astools_ctx *c = NULL;
  astools_result r = {0};
  /* Restrict collection to Release even with a single-config generator: losing
   * CTest's -C while adding the JUnit report must fail on every CI platform. */
  const char *variants[] = {
    "cmake_minimum_required(VERSION 3.16)\nproject(proofs NONE)\nenable_testing()\nadd_test(NAME ok COMMAND ${CMAKE_COMMAND} -E true CONFIGURATIONS Release)\n",
    "cmake_minimum_required(VERSION 3.16)\nproject(proofs NONE)\nenable_testing()\nadd_test(NAME ok COMMAND ${CMAKE_COMMAND} -E true CONFIGURATIONS Release)\nset_tests_properties(ok PROPERTIES DISABLED TRUE)\n",
    "cmake_minimum_required(VERSION 3.16)\nproject(proofs NONE)\nenable_testing()\nadd_test(NAME fails COMMAND ${CMAKE_COMMAND} -E false CONFIGURATIONS Release)\n",
    "cmake_minimum_required(VERSION 3.16)\nproject(proofs NONE)\nenable_testing()\n",
    "cmake_minimum_required(VERSION 3.16)\nproject(proofs NONE)\nmessage(FATAL_ERROR \"fixture configuration failure\")\n"
  };
  const char *statuses[] = {"passed", "not_run", "failed", "failed", "failed"};
  size_t i;
  ASSERT_TRUE(astools_test_tmpdir(ws));
  for (i = 0; ws[i]; i++) if (ws[i] == '\\') ws[i] = '/';
  snprintf(path, sizeof path, "%s/CMakeLists.txt", ws);
  snprintf(config_path, sizeof config_path, "%s/grant.xcdn", ws);
  snprintf(config, sizeof config,
      "#astools_config {registry:{paths:[{path:\"%s\",trust:\"standard\"}],watch:\"off\",pinning:\"off\"},"
      "sandbox:{executable_paths:[\"%s\"]},workspace:{root:\"%s\"},grants:{workspace_access:\"read-write\",tools:[{tool:\"project\",proc:true}]}}",
      ASTOOLS_STD_PACKAGES, ASTOOLS_TEST_CMAKE_BIN, ws);
  ASSERT_TRUE(write_text(config_path, config));
  op.config_path = config_path;
  ASSERT_OK(astools_open(&op, &c));
  for (i = 0; i < sizeof variants / sizeof *variants; i++) {
    ASSERT_TRUE(write_text(path, variants[i]));
    ASSERT_OK(astools_invoke(c, "project", "test", "{}", 0, &r));
    if (!r.ok) fprintf(stderr, "project.test: %s: %s\n",
        r.error_code ? r.error_code : "", r.error_message ? r.error_message : "");
    ASSERT_EQ_INT(r.ok, 1);
    ASSERT_TRUE(r.result_xcdn != NULL);
    xcdn_document_t *doc = xcdn_parse(r.result_xcdn, NULL);
    ASSERT_TRUE(doc && doc->values_len == 1);
    const xcdn_node_t *status = xcdn_object_get(doc->values[0]->value, "verification_status");
    ASSERT_TRUE(status && status->value->type == XCDN_VAL_STRING);
    if (i == 1) {
      /* CTest versions differ on whether --no-tests=error rejects a collection
       * containing only disabled tests; neither outcome may claim success. */
      ASSERT_TRUE(!strcmp(status->value->data.string, "not_run") ||
                  !strcmp(status->value->data.string, "failed"));
    } else {
      if (strcmp(status->value->data.string, statuses[i])) fprintf(stderr, "%s\n", r.result_xcdn);
      ASSERT_EQ_STR(status->value->data.string, statuses[i]);
    }
    if (i < 3 || i == 4) {
      const xcdn_node_t *collected = xcdn_object_get(doc->values[0]->value, "tests_collected");
      const xcdn_node_t *skipped = xcdn_object_get(doc->values[0]->value, "tests_skipped");
      ASSERT_TRUE(collected && collected->value->type == XCDN_VAL_INT);
      ASSERT_TRUE(skipped && skipped->value->type == XCDN_VAL_INT);
      if (i == 4) ASSERT_EQ_INT(collected->value->data.integer, -1);
      if (i != 0 && collected->value->data.integer == -1) {
        /* A failed/disabled run need not produce a usable JUnit report on
         * every CTest/generator combination. Unknown counts must stay unknown
         * and cannot certify success. The positive fixture always needs proof. */
        ASSERT_EQ_INT(skipped->value->data.integer, -1);
        ASSERT_EQ_STR(status->value->data.string, "failed");
      } else {
        if (collected->value->data.integer != 1 ||
            skipped->value->data.integer != (i == 1 ? 1 : 0))
          fprintf(stderr, "project.test variant %zu: %s\n", i, r.result_xcdn);
        ASSERT_EQ_INT(collected->value->data.integer, 1);
        ASSERT_EQ_INT(skipped->value->data.integer, i == 1 ? 1 : 0);
      }
    }
    xcdn_document_free(doc);
    astools_result_free(&r);
  }
  astools_close(c); astools_test_rmtree(ws);
}

TEST_LIST = {
  TEST_ENTRY(edits_reject_stale_versions),
  TEST_ENTRY(project_test_reports_real_collection),
  TEST_ENTRY(code_commands_and_proc_isolation),
  TEST_ENTRY(edit_patch_relocates_unique_stale_hunk),
};

RUN_ALL_TESTS()
