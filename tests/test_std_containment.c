/*
 * test_std_containment.c — security regressions for the std tools' own
 * containment (the paths the runtime pre-flight cannot see):
 *   - edit.patch resolves diff-internal target paths through the kernel and
 *     refuses a workspace symlink that escapes the tree.
 *   - fs.copy never follows a destination symlink during recursion.
 * Both run against the REAL astools-std packages (ASTOOLS_STD_PACKAGES).
 */

#include "astools_test.h"

#include <sys/stat.h>
#include <unistd.h>

#include "astools.h"
#include "os.h"

#ifndef ASTOOLS_STD_PACKAGES
#define ASTOOLS_STD_PACKAGES "packages"
#endif

/* Whole-file read into a malloc'd NUL-terminated buffer; NULL if absent. */
static char *slurp(const char *path) {
  return astools_test_read_file(path, NULL);
}

static int write_file(const char *path, const char *data) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  fputs(data, f);
  fclose(f);
  return 1;
}

TEST(edit_patch_symlink_escape_is_contained) {
  char root[256], ws[512], outside[512], p[1024], target[1024];
  const char *roots[2];
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  astools_err e;
  char *content;

  ASSERT_TRUE(astools_test_tmpdir(root));
  snprintf(ws, sizeof ws, "%s/ws", root);
  snprintf(outside, sizeof outside, "%s/outside", root);
  mkdir(ws, 0755);
  mkdir(outside, 0755);
  snprintf(target, sizeof target, "%s/secret.txt", outside);
  ASSERT_TRUE(write_file(target, "SECRET-ORIGINAL\n"));
  /* plant a workspace symlink pointing outside */
  snprintf(p, sizeof p, "%s/escape", ws);
  ASSERT_TRUE(symlink(outside, p) == 0);

  roots[0] = ASTOOLS_STD_PACKAGES;
  roots[1] = NULL;
  memset(&op, 0, sizeof op);
  op.registry_paths = roots;
  op.workspace_root = ws;
  ASSERT_OK(astools_open(&op, &c));

  e = astools_invoke(
      c, "edit", "patch",
      "{patch: \"--- a/escape/secret.txt\\n+++ b/escape/secret.txt\\n"
      "@@ -1 +1 @@\\n-SECRET-ORIGINAL\\n+PWNED\\n\"}",
      0, &r);
  /* engine accepts the call; the tool must report a containment failure */
  ASSERT_EQ_INT(e, ASTOOLS_OK);
  ASSERT_EQ_INT(r.ok, 0);
  astools_result_free(&r);

  /* the external file must be byte-for-byte intact */
  content = slurp(target);
  ASSERT_TRUE(content != NULL);
  ASSERT_EQ_STR(content, "SECRET-ORIGINAL\n");
  free(content);

  astools_close(c);
  astools_test_rmtree(root);
}

TEST(fs_copy_destination_symlink_is_contained) {
  char root[256], ws[512], outside[512], p[1024], target[1024];
  const char *roots[2];
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  astools_err e;
  char *content;

  ASSERT_TRUE(astools_test_tmpdir(root));
  snprintf(ws, sizeof ws, "%s/ws", root);
  snprintf(outside, sizeof outside, "%s/outside", root);
  mkdir(ws, 0755);
  mkdir(outside, 0755);
  snprintf(target, sizeof target, "%s/secret.txt", outside);
  ASSERT_TRUE(write_file(target, "SECRET-ORIGINAL\n"));

  /* src dir with one file whose name collides with a dest symlink */
  snprintf(p, sizeof p, "%s/srcdir", ws);
  mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/srcdir/secret.txt", ws);
  ASSERT_TRUE(write_file(p, "payload\n"));
  snprintf(p, sizeof p, "%s/destdir", ws);
  mkdir(p, 0755);
  /* destdir/secret.txt -> outside/secret.txt */
  snprintf(p, sizeof p, "%s/destdir/secret.txt", ws);
  ASSERT_TRUE(symlink(target, p) == 0);

  roots[0] = ASTOOLS_STD_PACKAGES;
  roots[1] = NULL;
  memset(&op, 0, sizeof op);
  op.registry_paths = roots;
  op.workspace_root = ws;
  ASSERT_OK(astools_open(&op, &c));

  e = astools_invoke(c, "fs", "copy",
                     "{src: \"srcdir\", dst: \"destdir\", recursive: true}", 0,
                     &r);
  ASSERT_EQ_INT(e, ASTOOLS_OK);
  ASSERT_EQ_INT(r.ok, 0); /* must refuse to write through the symlink */
  astools_result_free(&r);

  content = slurp(target);
  ASSERT_TRUE(content != NULL);
  ASSERT_EQ_STR(content, "SECRET-ORIGINAL\n");
  free(content);

  astools_close(c);
  astools_test_rmtree(root);
}

TEST(edit_patch_legit_in_workspace_still_applies) {
  char root[256], ws[512], p[1024];
  const char *roots[2];
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  char *content;

  ASSERT_TRUE(astools_test_tmpdir(root));
  snprintf(ws, sizeof ws, "%s/ws", root);
  mkdir(ws, 0755);
  snprintf(p, sizeof p, "%s/file.txt", ws);
  ASSERT_TRUE(write_file(p, "line1\nline2\nline3\n"));

  roots[0] = ASTOOLS_STD_PACKAGES;
  roots[1] = NULL;
  memset(&op, 0, sizeof op);
  op.registry_paths = roots;
  op.workspace_root = ws;
  ASSERT_OK(astools_open(&op, &c));

  ASSERT_OK(astools_invoke(
      c, "edit", "patch",
      "{patch: \"--- a/file.txt\\n+++ b/file.txt\\n@@ -1,3 +1,3 @@\\n line1\\n"
      "-line2\\n+LINE-TWO\\n line3\\n\"}",
      0, &r));
  ASSERT_EQ_INT(r.ok, 1);
  astools_result_free(&r);

  content = slurp(p);
  ASSERT_TRUE(content != NULL);
  ASSERT_EQ_STR(content, "line1\nLINE-TWO\nline3\n");
  free(content);

  astools_close(c);
  astools_test_rmtree(root);
}

TEST(proc_timeout_does_not_wait_for_orphan_pipe_holders) {
  char root[256], ws[512], cfg_path[512], cfg[4096];
  astools_open_params op;
  astools_ctx *c = NULL;
  astools_result r;
  int64_t start, elapsed;

  ASSERT_TRUE(astools_test_tmpdir(root));
  snprintf(ws, sizeof ws, "%s/ws", root);
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", root);
  ASSERT_TRUE(mkdir(ws, 0755) == 0);
  snprintf(cfg, sizeof cfg,
           "#astools_config {\n"
           " registry: { paths: [ { path: \"%s\", trust: \"standard\" } ],"
           " watch: \"off\", pinning: \"off\" },\n"
           " workspace: { root: \"%s\" },\n"
           " grants: { workspace_access: \"read-write\", tools: ["
           " { tool: \"proc\", proc: true } ] },\n"
           "}\n",
           ASTOOLS_STD_PACKAGES, ws);
  ASSERT_TRUE(write_file(cfg_path, cfg));
  memset(&op, 0, sizeof op);
  op.config_path = cfg_path;
  ASSERT_OK(astools_open(&op, &c));

  memset(&r, 0, sizeof r);
  start = os_monotonic_ms();
  ASSERT_OK(astools_invoke(
      c, "proc", "run",
      "{ argv: [\"/bin/sh\", \"-c\", \"sleep 4 &\"], timeout: r\"PT1S\" }",
      3000, &r));
  elapsed = os_monotonic_ms() - start;
  ASSERT_EQ_INT(r.ok, 1);
  ASSERT_TRUE(elapsed < 2500);
  astools_result_free(&r);
  astools_close(c);
  astools_test_rmtree(root);
}

TEST_LIST = {
  TEST_ENTRY(edit_patch_symlink_escape_is_contained),
  TEST_ENTRY(fs_copy_destination_symlink_is_contained),
  TEST_ENTRY(edit_patch_legit_in_workspace_still_applies),
  TEST_ENTRY(proc_timeout_does_not_wait_for_orphan_pipe_holders),
};

RUN_ALL_TESTS()
