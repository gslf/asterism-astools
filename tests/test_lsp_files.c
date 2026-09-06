/* URI, version and authorization checks do not need an installed language server. */
#include "astools_test.h"
#include "lsp_results.h"
#ifndef _WIN32
#include <fcntl.h>
#endif

static jx_value *json(const char *text) {
  jx_value *v = NULL;
  (void)jx_parse(text, strlen(text), &v);
  return v;
}
TEST(uri_and_utf8_positions) {
  char *uri = astools_lsp_uri("/tmp/è #?.cpp");
  ASSERT_EQ_STR(uri, "file:///tmp/%C3%A8%20%23%3F.cpp");
  char *path = astools_lsp_path(uri);
  ASSERT_EQ_STR(path, "/tmp/è #?.cpp");
  free(path);
  free(uri);
  const char *bad[] = {"file://host/tmp/a", "file:///a%00b", "file:///a%5cb", "file:///a?b",
                       "file:///a#b",       "file:///a%",    "file:///a%ff"};
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) ASSERT_TRUE(!astools_lsp_path(bad[i]));
  astools_lsp_file f = {0};
  f.text = "è😀x\r\nz\n";
  f.len = strlen(f.text);
  const char *positions[] = {"{\"line\":0,\"character\":6}", "{\"line\":0,\"character\":1}",
                             "{\"line\":0,\"character\":7}", "{\"line\":0,\"character\":8}",
                             "{\"line\":2,\"character\":0}", "{\"line\":3,\"character\":0}",
                             "{\"line\":-1,\"character\":0}"};
  int valid[] = {1, 0, 1, 0, 1, 0, 0};
  for (size_t i = 0; i < sizeof valid / sizeof *valid; i++) {
    jx_value *p = json(positions[i]);
    size_t offset;
    int ok = astools_lsp_offset(&f, p, &offset);
    jx_free(p);
    ASSERT_EQ_INT(ok, valid[i]);
  }
}
#ifndef _WIN32
static int canonical_tmpdir(char root[256]) {
  if (!astools_test_tmpdir(root)) return 0;
  char *canonical = realpath(root, NULL);
  if (!canonical || strlen(canonical) >= 256) { free(canonical); return 0; }
  strcpy(root, canonical); free(canonical); return 1;
}
TEST(read_grants_versions_and_aliases) {
  char root[256], file[512], alias[512], nested[512];
  ASSERT_TRUE(canonical_tmpdir(root));
  snprintf(file, sizeof file, "%s/a.cpp", root);
  snprintf(alias, sizeof alias, "%s/link", root);
  snprintf(nested, sizeof nested, "%s/link/a.cpp", root);
  ASSERT_OK(os_write_file(file, "int a;\n", 7));
  astools_fs_perm fs = {root, ASTOOLS_ACCESS_READ};
  astools_effective eff = {0};
  eff.fs = &fs;
  eff.fs_len = 1;
  astools_lsp_file out;
  ASSERT_OK(astools_lsp_file_read(root, file, &eff, &out));
  ASSERT_EQ_STR(out.text, "int a;\n");
  ASSERT_EQ_INT(strlen(out.sha256), 64);
  astools_lsp_file_free(&out);
  fs.access = ASTOOLS_ACCESS_WRITE;
  ASSERT_ERR(astools_lsp_file_read(root, file, &eff, &out), ASTOOLS_ERR_DENIED);
  fs.access = ASTOOLS_ACCESS_READ;
  ASSERT_EQ_INT(symlink(file, alias), 0);
  ASSERT_ERR(astools_lsp_file_read(root, alias, &eff, &out), ASTOOLS_ERR_DENIED);
  unlink(alias);
  ASSERT_EQ_INT(symlink(root, alias), 0);
  ASSERT_ERR(astools_lsp_file_read(root, nested, &eff, &out), ASTOOLS_ERR_DENIED);
  unlink(alias);
  ASSERT_EQ_INT(mkfifo(alias, 0600), 0);
  ASSERT_ERR(astools_lsp_file_read(root, alias, &eff, &out), ASTOOLS_ERR_DENIED);
  ASSERT_ERR(astools_lsp_file_read(root, "/etc/passwd", &eff, &out), ASTOOLS_ERR_DENIED);
  ASSERT_OK(os_write_file(file, "a\0b", 3));
  ASSERT_ERR(astools_lsp_file_read(root, file, &eff, &out), ASTOOLS_ERR_INVALID);
  ASSERT_OK(os_write_file(file, "\xff", 1));
  ASSERT_ERR(astools_lsp_file_read(root, file, &eff, &out), ASTOOLS_ERR_INVALID);
  ASSERT_EQ_INT(truncate(file, LSP_FILE_BYTES + 1), 0);
  ASSERT_ERR(astools_lsp_file_read(root, file, &eff, &out), ASTOOLS_ERR_TOOL);
  astools_test_rmtree(root);
}
TEST(root_ancestors_cannot_redirect_internal_reads) {
  char root[256], directory[512], file[512], alias[512], aliased_root[512], aliased_file[512];
  ASSERT_TRUE(canonical_tmpdir(root));
  snprintf(directory, sizeof directory, "%s/sub", root);
  snprintf(file, sizeof file, "%s/sub/a.cpp", root);
  snprintf(alias, sizeof alias, "%s/alias", root);
  snprintf(aliased_root, sizeof aliased_root, "%s/alias/sub", root);
  snprintf(aliased_file, sizeof aliased_file, "%s/alias/sub/a.cpp", root);
  ASSERT_EQ_INT(mkdir(directory, 0700), 0);
  ASSERT_OK(os_write_file(file, "int a;", 6));
  ASSERT_EQ_INT(symlink(root, alias), 0);
  astools_fs_perm fs = {aliased_root, ASTOOLS_ACCESS_READ};
  astools_effective eff = {0};
  eff.fs = &fs;
  eff.fs_len = 1;
  astools_lsp_file out;
  astools_err e = astools_lsp_file_read(aliased_root, aliased_file, &eff, &out);
  astools_lsp_file_free(&out);
  astools_test_rmtree(root);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_DENIED);
}
TEST(results_reject_stale_files_and_invalid_ranges) {
  char root[256], file[512];
  ASSERT_TRUE(canonical_tmpdir(root));
  snprintf(file, sizeof file, "%s/a.cpp", root);
  ASSERT_OK(os_write_file(file, "int a;\n", 7));
  astools_ctx c = {0};
  c.workspace = root;
  astools_fs_perm fs = {root, ASTOOLS_ACCESS_READ};
  astools_effective eff = {0};
  eff.fs = &fs;
  eff.fs_len = 1;
  const char *inputs[] = {
      "[{\"name\":\"a\",\"kind\":13,\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
      "\"line\":0,\"character\":6}}}]",
      "[{\"name\":\"a\",\"kind\":13,\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
      "\"line\":4,\"character\":0}}}]",
      "[{\"name\":\"a\",\"kind\":13,\"location\":{\"range\":{\"start\":{\"line\":0,\"character\":0}"
      ",\"end\":{\"line\":0,\"character\":1}}}}]",
      "[{\"name\":\"a\",\"kind\":13,\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
      "\"line\":0,\"character\":1}},\"selectionRange\":{\"start\":{\"line\":0,\"character\":0},"
      "\"end\":{\"line\":0,\"character\":6}}}]"};
  for (size_t i = 0; i < sizeof inputs / sizeof *inputs; i++) {
    astools_lsp_view view = {0};
    view.ctx = &c;
    view.grants = &eff;
    ASSERT_OK(astools_lsp_file_read(root, file, &eff, &view.files[0]));
    view.count = 1;
    jx_value *reply = json(inputs[i]);
    astools_err e = astools_lsp_render(&view, "symbols", reply);
    jx_free(reply);
    if (!i) {
      ASSERT_OK(e);
      ASSERT_OK(astools_lsp_recheck(&view));
      ASSERT_OK(os_write_file(file, "int b;\n", 7));
      ASSERT_ERR(astools_lsp_recheck(&view), ASTOOLS_ERR_BUSY);
    } else ASSERT_EQ_INT(e, ASTOOLS_ERR_PROTOCOL);
    astools_lsp_view_free(&view);
  }
  astools_test_rmtree(root);
}
#endif
TEST_LIST = {
    TEST_ENTRY(uri_and_utf8_positions),
#ifndef _WIN32
    TEST_ENTRY(read_grants_versions_and_aliases),
    TEST_ENTRY(root_ancestors_cannot_redirect_internal_reads),
    TEST_ENTRY(results_reject_stale_files_and_invalid_ranges),
#endif
};
RUN_ALL_TESTS()
