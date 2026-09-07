/* Loading a real library must not bypass package identity or response correlation. */
#include "astools_test.h"
#include "astools_internal.h"
#include "fakes.h"

typedef struct {
  char root[256], config[512];
  astools_ctx *c;
} fixture;
static int package(fixture *f, const char *revision) {
  if (!fake_registry_write(f->root, "lib", ASTOOLS_TEST_LIBRARY_PATH, revision, NULL,
                           FAKE_CMD_RUN_ECHO, NULL))
    return 0;
  char path[512];
  snprintf(path, sizeof path, "%s/lib/manifest.xcdn", f->root);
  char *text = NULL;
  size_t n = 0;
  if (os_read_file(path, &text, &n) != ASTOOLS_OK) return 0;
  char *kind = strstr(text, "kind: \"executable\"");
  astools_buf b = {0};
  int ok = kind && astools_buf_append(&b, text, (size_t)(kind - text)) == ASTOOLS_OK &&
           astools_buf_appends(&b, "kind: \"library\"") == ASTOOLS_OK &&
           astools_buf_appends(&b, kind + strlen("kind: \"executable\"")) == ASTOOLS_OK &&
           os_write_file(path, b.data, b.len) == ASTOOLS_OK;
  free(text);
  astools_buf_free(&b);
  return ok;
}
static int setup(fixture *f) {
  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->root) || !package(f, "first")) return 0;
  snprintf(f->config, sizeof f->config, "%s/config.xcdn", f->root);
  if (!fake_config_write(f->config, f->root, 1, f->root, "sandbox: {allow_library:true},"))
    return 0;
  astools_open_params p = {0};
  p.config_path = f->config;
  return astools_open(&p, &f->c) == ASTOOLS_OK;
}
static void drop(fixture *f) {
  astools_close(f->c);
  astools_test_rmtree(f->root);
}
TEST(loaded_library_change_requires_reopen) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  astools_result r = {0};
  ASSERT_OK(astools_invoke(f.c, "lib", "run", "{msg:\"hello\"}", 500, &r));
  ASSERT_TRUE(r.ok);
  astools_result_free(&r);
  ASSERT_TRUE(package(&f, "second"));
  ASSERT_OK(astools_registry_refresh(f.c));
  astools_err e = astools_invoke(f.c, "lib", "run", "{msg:\"hello\"}", 500, &r);
  astools_result_free(&r);
  astools_close(f.c);
  f.c = NULL;
  astools_open_params p = {0};
  p.config_path = f.config;
  ASSERT_OK(astools_open(&p, &f.c));
  astools_err reopened = astools_invoke(f.c, "lib", "run", "{msg:\"hello\"}", 500, &r);
  int good = r.ok;
  astools_result_free(&r);
  drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_DENIED);
  ASSERT_EQ_INT(reopened, ASTOOLS_OK);
  ASSERT_TRUE(good);
}
TEST(wrong_request_result_is_rejected) {
  fixture f;
  ASSERT_TRUE(setup(&f));
  astools_result r = {0};
  astools_err e = astools_invoke(f.c, "lib", "run", "{msg:\"_wrong_request_\"}", 500, &r);
  int ok = r.ok;
  astools_result_free(&r);
  drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_PROTOCOL);
  ASSERT_TRUE(!ok);
}
TEST_LIST = {TEST_ENTRY(loaded_library_change_requires_reopen),
             TEST_ENTRY(wrong_request_result_is_rejected)};
RUN_ALL_TESTS()
