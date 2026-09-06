/* Real subprocesses exercise residency, sandbox lifetime and queued deadlines. */
#include "astools_test.h"
#include "execution.h"
#include "fakes.h"

typedef struct {
  char root[256], config[512];
  astools_ctx *c;
} fixture;
static int package(fixture *f, const char *mode) {
  if (!fake_registry_write(f->root, "pp", ASTOOLS_TEST_PERSISTENT_PATH, mode, NULL, FAKE_CMD_RUN_MS,
                           NULL))
    return 0;
  char path[512];
  snprintf(path, sizeof path, "%s/pp/manifest.xcdn", f->root);
  char *text = NULL;
  size_t n = 0;
  if (os_read_file(path, &text, &n) != ASTOOLS_OK) return 0;
  char *start = strstr(text, "mode: \"oneshot\"");
  astools_buf b = {0};
  int ok = start && astools_buf_append(&b, text, (size_t)(start - text)) == ASTOOLS_OK &&
           astools_buf_appends(&b, "mode: \"persistent\"") == ASTOOLS_OK &&
           astools_buf_appends(&b, start + strlen("mode: \"oneshot\"")) == ASTOOLS_OK &&
           os_write_file(path, b.data, b.len) == ASTOOLS_OK;
  astools_buf_free(&b);
  free(text);
  return ok;
}
static int setup(fixture *f, const char *mode, const char *level) {
  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->root) || !package(f, mode)) return 0;
  snprintf(f->config, sizeof f->config, "%s/config.xcdn", f->root);
  char extra[256];
  snprintf(extra, sizeof extra, "sandbox: {default_level:\"%s\", strict_fallback:\"reject\"},",
           level);
  if (!fake_config_write(f->config, f->root, 1, f->root, extra)) return 0;
  astools_open_params p = {0};
  p.config_path = f->config;
  return astools_open(&p, &f->c) == ASTOOLS_OK;
}
static void drop(fixture *f) {
  astools_close(f->c);
  astools_test_rmtree(f->root);
}
static const xcdn_value_t *field(const xcdn_value_t *v, const char *key) {
  const xcdn_node_t *n = xcdn_object_get(v, key);
  return n ? n->value : NULL;
}
static xcdn_document_t *call(fixture *f, int ms) {
  char args[64];
  snprintf(args, sizeof args, "{ms:%d}", ms);
  astools_result r = {0};
  astools_err e = astools_invoke(f->c, "pp", "run", args, 2000, &r);
  xcdn_document_t *doc = NULL;
  xcdn_error_t error;
  if (e == ASTOOLS_OK && r.ok)
    doc = xcdn_parse(r.result_xcdn, &error);
  else
    fprintf(stderr, "peer failed: %s %s\n", astools_err_name(e),
            r.error_message ? r.error_message : "");
  astools_result_free(&r);
  return doc;
}
static void scratch_case(const char *level) {
  fixture f;
  ASSERT_TRUE(setup(&f, "first", level));
  xcdn_document_t *one = call(&f, 0), *two = call(&f, 0);
  int good = one && two && field(one->values[0]->value, "scratch_ok")->data.boolean &&
             field(two->values[0]->value, "scratch_ok")->data.boolean &&
             field(two->values[0]->value, "calls")->data.integer == 2;
  char *path = two ? astools_strdup(field(two->values[0]->value, "scratch")->data.string) : NULL;
  xcdn_document_free(one);
  xcdn_document_free(two);
  astools_close(f.c);
  f.c = NULL;
  int removed = path && !os_file_exists(path);
  free(path);
  drop(&f);
  ASSERT_TRUE(good);
  ASSERT_TRUE(removed);
}
TEST(scratch_lives_with_process) { scratch_case("basic"); }
TEST(strict_scratch_lives_with_process) {
  astools_sandbox_caps caps = {0};
  ASSERT_OK(astools_sandbox_caps_impl(1, &caps));
  if (caps.fs_confinement) scratch_case("strict");
}
TEST(replacement_cannot_reuse_old_process) {
  fixture f;
  ASSERT_TRUE(setup(&f, "first", "basic"));
  xcdn_document_t *one = call(&f, 0);
  ASSERT_TRUE(one);
  xcdn_document_free(one);
  ASSERT_TRUE(package(&f, "second"));
  ASSERT_OK(astools_registry_refresh(f.c));
  xcdn_document_t *two = call(&f, 0);
  int generation = two ? (int)field(two->values[0]->value, "generation")->data.integer : 0;
  xcdn_document_free(two);
  drop(&f);
  ASSERT_EQ_INT(generation, 2);
}
TEST(startup_obeys_request_deadline) {
  fixture f;
  ASSERT_TRUE(setup(&f, "slow", "basic"));
  astools_result r = {0};
  int64_t begin = os_monotonic_ms();
  astools_err e = astools_invoke(f.c, "pp", "run", "{ms:0}", 80, &r);
  int64_t elapsed = os_monotonic_ms() - begin;
  astools_result_free(&r);
  drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_TIMEOUT);
  ASSERT_TRUE(elapsed < 500);
}
static int accepted(fixture *f) {
  char path[512];
  snprintf(path, sizeof path, "%s/accepted", f->root);
  for (int i = 0; i < 200; i++) {
    if (os_file_exists(path)) return 1;
    astools_exec_sleep(5);
  }
  return 0;
}
static int waiting(fixture *f) {
  for (int i = 0; i < 100; i++) {
    astools_stats stats = {0};
    astools_get_stats(f->c, &stats);
    if (stats.instance_waiters) return 1;
    astools_exec_sleep(1);
  }
  return 0;
}
static void queue_case(int cancel) {
  fixture f;
  ASSERT_TRUE(setup(&f, "first", "basic"));
  astools_task *first = NULL, *second = NULL;
  astools_result r = {0};
  ASSERT_OK(astools_invoke_async(f.c, "pp", "run", "{ms:700}", 2000, &first));
  ASSERT_TRUE(accepted(&f));
  int64_t begin = os_monotonic_ms();
  ASSERT_OK(astools_invoke_async(f.c, "pp", "run", "{ms:0}", cancel ? 2000 : 200, &second));
  int queued = waiting(&f);
  if (cancel) ASSERT_OK(astools_task_cancel(second));
  astools_err e = astools_task_wait(second, 400, &r);
  int64_t elapsed = os_monotonic_ms() - begin;
  astools_result_free(&r);
  astools_task_free(second);
  astools_task_wait(first, 2000, &r);
  astools_result_free(&r);
  astools_task_free(first);
  xcdn_document_t *last = call(&f, 0);
  int calls = last ? (int)field(last->values[0]->value, "calls")->data.integer : 0;
  xcdn_document_free(last);
  drop(&f);
  ASSERT_EQ_INT(e, cancel ? ASTOOLS_ERR_CANCELLED : ASTOOLS_ERR_TIMEOUT);
  ASSERT_TRUE(queued);
  ASSERT_TRUE(elapsed < 400);
  ASSERT_EQ_INT(calls, 2);
}
TEST(instance_queue_expires_without_effects) { queue_case(0); }
TEST(instance_queue_cancels_without_effects) { queue_case(1); }
TEST(shutdown_kills_remaining_group) {
  fixture f;
  ASSERT_TRUE(setup(&f, "tail", "basic"));
  xcdn_document_t *doc = call(&f, 0);
  ASSERT_TRUE(doc);
  xcdn_document_free(doc);
  astools_close(f.c);
  f.c = NULL;
  astools_exec_sleep(800);
  char path[512];
  snprintf(path, sizeof path, "%s/survived", f.root);
  int survived = os_file_exists(path);
  drop(&f);
  ASSERT_TRUE(!survived);
}
TEST(revoked_idle_process_is_reaped) {
  fixture f;
  ASSERT_TRUE(setup(&f, "first", "basic"));
  xcdn_document_t *doc = call(&f, 0);
  ASSERT_TRUE(doc);
  xcdn_document_free(doc);
  ASSERT_OK(astools_tool_enable(f.c, "pp", 0));
  ASSERT_OK(astools_tick(f.c));
  astools_stats stats = {0};
  ASSERT_OK(astools_get_stats(f.c, &stats));
  drop(&f);
  ASSERT_EQ_INT(stats.instance_slots, 0);
}
TEST(changed_package_after_instance_queue_is_rejected) {
  fixture f;
  ASSERT_TRUE(setup(&f, "first", "basic"));
  astools_task *first = NULL, *second = NULL;
  astools_result r = {0};
  ASSERT_OK(astools_invoke_async(f.c, "pp", "run", "{ms:700}", 2000, &first));
  ASSERT_TRUE(accepted(&f));
  ASSERT_OK(astools_invoke_async(f.c, "pp", "run", "{ms:0}", 2000, &second));
  int queued = waiting(&f);
  ASSERT_TRUE(package(&f, "second"));
  ASSERT_OK(astools_registry_refresh(f.c));
  astools_err e = astools_task_wait(second, 2000, &r);
  astools_result_free(&r);
  astools_task_free(second);
  astools_task_wait(first, 2000, &r);
  astools_result_free(&r);
  astools_task_free(first);
  drop(&f);
  ASSERT_TRUE(queued);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_DENIED);
}
TEST(wrong_response_fails_without_waiting_for_deadline) {
  fixture f;
  ASSERT_TRUE(setup(&f, "wrongid", "basic"));
  astools_result r = {0};
  int64_t begin = os_monotonic_ms();
  astools_err e = astools_invoke(f.c, "pp", "run", "{ms:0}", 1500, &r);
  int64_t elapsed = os_monotonic_ms() - begin;
  astools_result_free(&r);
  drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_PROTOCOL);
  ASSERT_TRUE(elapsed < 800);
}
TEST_LIST = {TEST_ENTRY(scratch_lives_with_process),
             TEST_ENTRY(strict_scratch_lives_with_process),
             TEST_ENTRY(replacement_cannot_reuse_old_process),
             TEST_ENTRY(startup_obeys_request_deadline),
             TEST_ENTRY(instance_queue_expires_without_effects),
             TEST_ENTRY(instance_queue_cancels_without_effects),
             TEST_ENTRY(shutdown_kills_remaining_group),
             TEST_ENTRY(revoked_idle_process_is_reaped),
             TEST_ENTRY(changed_package_after_instance_queue_is_rejected),
             TEST_ENTRY(wrong_response_fails_without_waiting_for_deadline)};
RUN_ALL_TESTS()
