/* Transport failures retire the actual managed process, without replay. */
#include "astools_test.h"
#include "lsp_fixture.h"
static void result_case(const char *mode, const char *command, astools_err expected) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, mode, "int a;\n", "basic"));
  jx_value *out = NULL;
  astools_err e =
      lsp_call(&f, command, !strcmp(command, "definition") ? ",line:1,column:5" : NULL, 1500, &out);
  jx_free(out);
  lsp_drop(&f);
  ASSERT_EQ_INT(e, expected);
}
TEST(correlation_and_negotiation) {
  result_case("normal", "symbols", ASTOOLS_OK);
  result_case("change", "symbols", ASTOOLS_ERR_BUSY);
  result_case("wrongid", "symbols", ASTOOLS_ERR_PROTOCOL);
  result_case("duplicate", "symbols", ASTOOLS_ERR_PROTOCOL);
  result_case("utf16", "symbols", ASTOOLS_ERR_UNSUPPORTED);
}
TEST(large_output_limit_does_not_overflow_io) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "normal", "int a;\n", "basic"));
  f.c->cfg.max_output_bytes = INT64_MAX;
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "symbols", NULL, 1500, &out);
  jx_free(out);
  lsp_drop(&f);
  ASSERT_OK(e);
}
TEST(server_error_is_owned_by_the_request) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "error", "int a;\n", "basic"));
  char args[1024];
  snprintf(args, sizeof args, "%s}", f.args);
  astools_result r = {0};
  astools_err e = astools_invoke(f.c, "lsp", "symbols", args, 1500, &r);
  int good = r.error_message && !strncmp(r.error_message, "Server error -32801: ", 20) &&
             jx_utf8_valid(r.error_message, strlen(r.error_message));
  astools_result_free(&r);
  lsp_drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_BUSY);
  ASSERT_TRUE(good);
}
TEST(server_requests_cannot_edit) { result_case("request", "symbols", ASTOOLS_OK); }
TEST(diagnostics_require_current_version) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "diagnostics", "int a;\n", "basic"));
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "diagnostics", NULL, 1500, &out);
  int count = (int)jx_array_len(jx_object_get(out, "items"));
  jx_free(out);
  lsp_drop(&f);
  ASSERT_OK(e);
  ASSERT_EQ_INT(count, 1);
}
TEST(stale_diagnostics_never_become_an_empty_success) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "stale", "int a;\n", "basic"));
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "diagnostics", NULL, 200, &out);
  int returned = out != NULL;
  jx_free(out);
  lsp_drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_TIMEOUT);
  ASSERT_TRUE(!returned);
}
TEST(external_locations_are_excluded) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "outside", "int a;\n", "basic"));
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "definition", ",line:1,column:5", 1500, &out);
  int count = (int)jx_array_len(jx_object_get(out, "items")),
      excluded = (int)jx_int_value(jx_object_get(out, "excluded"));
  jx_free(out);
  lsp_drop(&f);
  ASSERT_OK(e);
  ASSERT_EQ_INT(count, 0);
  ASSERT_EQ_INT(excluded, 1);
}
TEST(stale_expected_hash_is_a_conflict) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "normal", "int a;\n", "basic"));
  ASSERT_OK(os_write_file(f.file, "int b;\n", 7));
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "symbols", NULL, 1500, &out);
  jx_free(out);
  lsp_drop(&f);
  ASSERT_EQ_INT(e, ASTOOLS_ERR_BUSY);
}
TEST(deadline_and_cancel_retire_server) {
  for (int cancel = 0; cancel < 2; cancel++) {
    lsp_fixture f;
    ASSERT_TRUE(lsp_setup(&f, ASTOOLS_TEST_LSP_PATH, "hang", "int a;\n", "basic"));
    char args[1024], marker[512];
    snprintf(args, sizeof args, "%s}", f.args);
    snprintf(marker, sizeof marker, "%s/accepted", f.root);
    astools_task *task = NULL;
    ASSERT_OK(astools_invoke_async(f.c, "lsp", "symbols", args, cancel ? 2000 : 400, &task));
    for (int i = 0; i < 200 && !os_file_exists(marker); i++) astools_exec_sleep(5);
    int accepted = os_file_exists(marker);
    if (cancel) ASSERT_OK(astools_task_cancel(task));
    astools_result r = {0};
    astools_err e = astools_task_wait(task, 1500, &r);
    astools_result_free(&r);
    astools_task_free(task);
    int alive = 0;
    for (astools_pproc *p = f.c->pprocs; p; p = p->next)
      if (p->alive) alive++;
    jx_value *out = NULL;
    astools_err restarted = lsp_call(&f, "symbols", NULL, 1500, &out);
    jx_free(out);
    lsp_drop(&f);
    ASSERT_OK(restarted);
    ASSERT_TRUE(accepted);
    ASSERT_EQ_INT(e, cancel ? ASTOOLS_ERR_CANCELLED : ASTOOLS_ERR_TIMEOUT);
    ASSERT_EQ_INT(alive, 0);
  }
}
TEST_LIST = {TEST_ENTRY(correlation_and_negotiation),
             TEST_ENTRY(large_output_limit_does_not_overflow_io),
             TEST_ENTRY(server_requests_cannot_edit),
             TEST_ENTRY(server_error_is_owned_by_the_request),
             TEST_ENTRY(diagnostics_require_current_version),
             TEST_ENTRY(external_locations_are_excluded),
             TEST_ENTRY(stale_diagnostics_never_become_an_empty_success),
             TEST_ENTRY(stale_expected_hash_is_a_conflict),
             TEST_ENTRY(deadline_and_cancel_retire_server)};
RUN_ALL_TESTS()
