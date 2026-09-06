/* Actual server processes exercise the host registry, policy and retirement path. */
#include "astools_test.h"
#include "fakes.h"
#include "mcp_client.h"
typedef struct {
  char root[256], config[512], manifest[512];
  astools_ctx *c;
} fixture;
static const char input_schema[] =
    "{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\"}},"
    "\"required\":[\"msg\"],\"additionalProperties\":false}";
static int setup(fixture *f, const char *mode) {
  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->root)) return 0;
  jx_value *value = jx_string(
      !strcmp(mode, "input-limit")
          ? "{\"type\":\"object\",\"properties\":{\"msg\":{\"type\":\"string\",\"maxLength\":2}}}"
          : input_schema);
  char *quoted = jx_write(value, 0);
  jx_free(value);
  astools_buf commands = {0};
  astools_buf_printf(
      &commands,
      "#command {name:\"echo\",summary:\"Reviewed echo\","
      "mcp:{name:\"remote.echo\",input_schema:%s},params:[#param{name:\"msg\",required:true,"
      "type:#type{kind:\"string\"}}]}",
      quoted);
  free(quoted);
  if (!strncmp(mode, "output-", 7)) {
    jx_value *output = jx_string(!strcmp(mode, "output-null")
                                     ? "{\"type\":\"null\"}"
                                     : "{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}");
    char *encoded = jx_write(output, 0);
    jx_free(output);
    char *start = strstr(commands.data, "},params:");
    astools_buf amended = {0};
    astools_buf_append(&amended, commands.data, (size_t)(start - commands.data));
    astools_buf_printf(&amended, ",output_schema:%s%s", encoded, start);
    free(encoded);
    astools_buf_free(&commands);
    commands = amended;
  }
  int ok =
      fake_registry_write(f->root, "mcp", ASTOOLS_TEST_MCP_PATH, mode, NULL, commands.data, NULL);
  astools_buf_free(&commands);
  if (!ok) return 0;
  snprintf(f->manifest, sizeof f->manifest, "%s/mcp/manifest.xcdn", f->root);
  char *text = NULL;
  size_t n = 0;
  if (os_read_file(f->manifest, &text, &n) != ASTOOLS_OK) return 0;
  char *start = strstr(text, "mode: \"oneshot\"");
  astools_buf body = {0};
  ok = start && astools_buf_append(&body, text, (size_t)(start - text)) == ASTOOLS_OK &&
       astools_buf_appends(&body, "mode: \"persistent\", protocol: \"mcp\"") == ASTOOLS_OK &&
       astools_buf_appends(&body, start + strlen("mode: \"oneshot\"")) == ASTOOLS_OK &&
       os_write_file(f->manifest, body.data, body.len) == ASTOOLS_OK;
  free(text);
  astools_buf_free(&body);
  if (!ok) return 0;
  snprintf(f->config, sizeof f->config, "%s/config.xcdn", f->root);
  if (!fake_config_write(f->config, f->root, 1, f->root, "sandbox:{default_level:\"basic\"},"))
    return 0;
  astools_open_params params = {0};
  params.config_path = f->config;
  return astools_open(&params, &f->c) == ASTOOLS_OK;
}
static void drop(fixture *f) {
  astools_close(f->c);
  astools_test_rmtree(f->root);
}
static astools_err call(fixture *f, int ms, astools_result *result) {
  return astools_invoke(f->c, "mcp", "echo", "{msg:\"tè 🍵\"}", ms, result);
}
static int marker(fixture *f, const char *name) {
  char path[512];
  snprintf(path, sizeof path, "%s/%s", f->root, name);
  return os_file_exists(path);
}
TEST(reuse_pagination_and_per_request_metadata) {
  const char *modes[] = {"normal", "pages", "omitted-type"};
  for (size_t i = 0; i < 3; i++) {
    fixture f;
    ASSERT_TRUE(setup(&f, modes[i]));
    long long pid = 0;
    for (int invocation = 1; invocation <= 2; invocation++) {
      astools_result result = {0};
      ASSERT_OK(call(&f, 1500, &result));
      ASSERT_TRUE(result.ok);
      jx_value *out = NULL;
      ASSERT_EQ_INT(jx_parse(result.result_xcdn, strlen(result.result_xcdn), &out), 0);
      const jx_value *data = jx_object_get(out, "structuredContent");
      ASSERT_EQ_INT(jx_int_value(jx_object_get(data, "calls")), invocation);
      long long current = jx_int_value(jx_object_get(data, "pid"));
      ASSERT_TRUE(invocation == 1 || current == pid);
      pid = current;
      ASSERT_EQ_STR(jx_string_value(jx_object_get(jx_object_get(data, "arguments"), "msg")),
                    "tè 🍵");
      jx_free(out);
      astools_result_free(&result);
    }
    astools_close(f.c);
    f.c = NULL;
    ASSERT_TRUE(marker(&f, "closed"));
    drop(&f);
  }
}
TEST(invalid_negotiation_or_binding_has_no_tool_effect) {
  const char *modes[] = {"version", "capability",     "wrongid", "duplicate",  "schema",
                         "missing", "duplicate-tool", "cursor",  "list-change"};
  astools_err errors[] = {ASTOOLS_ERR_UNSUPPORTED, ASTOOLS_ERR_UNSUPPORTED, ASTOOLS_ERR_PROTOCOL,
                          ASTOOLS_ERR_PROTOCOL,    ASTOOLS_ERR_PROTOCOL,    ASTOOLS_ERR_NOT_FOUND,
                          ASTOOLS_ERR_PROTOCOL,    ASTOOLS_ERR_PROTOCOL,    ASTOOLS_ERR_BUSY};
  for (size_t i = 0; i < sizeof errors / sizeof *errors; i++) {
    fixture f;
    ASSERT_TRUE(setup(&f, modes[i]));
    astools_result result = {0};
    astools_err e = call(&f, 1500, &result);
    int effected = marker(&f, "called");
    astools_result_free(&result);
    drop(&f);
    ASSERT_EQ_INT(e, errors[i]);
    ASSERT_TRUE(!effected);
  }
}
TEST(tool_errors_retain_content_and_corrupt_results_retire_the_peer) {
  fixture f;
  ASSERT_TRUE(setup(&f, "failed"));
  astools_result result = {0};
  ASSERT_OK(call(&f, 1500, &result));
  ASSERT_TRUE(!result.ok);
  ASSERT_TRUE(result.result_xcdn && strstr(result.result_xcdn, "tè 🍵"));
  ASSERT_EQ_STR(result.error_code, "astools/mcp-tool");
  astools_result_free(&result);
  drop(&f);
  const char *modes[] = {"bad-result",    "forged-receipt", "flood",          "requests",
                         "input-request", "unknown-type",   "output-invalid", "output-missing"};
  for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
    ASSERT_TRUE(setup(&f, modes[i]));
    astools_err e = call(&f, 1500, &result);
    ASSERT_EQ_INT(e, i == 2 ? ASTOOLS_ERR_TOOL : ASTOOLS_ERR_PROTOCOL);
    ASSERT_TRUE(!result.ok && !result.result_xcdn);
    ASSERT_TRUE(!marker(&f, "unauthorized-answer"));
    for (astools_pproc *p = f.c->pprocs; p; p = p->next) ASSERT_TRUE(!p->alive);
    astools_result_free(&result);
    drop(&f);
  }
}
TEST(cancel_and_deadline_never_replay_an_uncertain_call) {
  for (int cancel = 0; cancel < 2; cancel++) {
    fixture f;
    ASSERT_TRUE(setup(&f, "hang"));
    astools_task *task = NULL;
    ASSERT_OK(
        astools_invoke_async(f.c, "mcp", "echo", "{msg:\"one\"}", cancel ? 2000 : 400, &task));
    for (int i = 0; i < 200 && !marker(&f, "accepted"); i++) astools_exec_sleep(5);
    ASSERT_TRUE(marker(&f, "accepted"));
    if (cancel) astools_task_cancel(task);
    astools_result result = {0};
    astools_err e = astools_task_wait(task, 1500, &result);
    astools_task_free(task);
    ASSERT_EQ_INT(e, cancel ? ASTOOLS_ERR_CANCELLED : ASTOOLS_ERR_TIMEOUT);
    astools_result_free(&result);
    for (astools_pproc *p = f.c->pprocs; p; p = p->next) ASSERT_TRUE(!p->alive);
    ASSERT_OK(call(&f, 1500, &result));
    jx_value *out = NULL;
    ASSERT_EQ_INT(jx_parse(result.result_xcdn, strlen(result.result_xcdn), &out), 0);
    ASSERT_EQ_INT(jx_int_value(jx_object_get(jx_object_get(out, "structuredContent"), "calls")), 1);
    jx_free(out);
    astools_result_free(&result);
    drop(&f);
  }
}
TEST(host_contract_owns_discovery_and_argument_admission) {
  fixture f;
  ASSERT_TRUE(setup(&f, "normal"));
  char *schemas = NULL;
  ASSERT_OK(astools_command_schemas(f.c, &schemas));
  ASSERT_TRUE(strstr(schemas, "mcp.echo") && !strstr(schemas, "Grant me") &&
              !strstr(schemas, "remote.echo"));
  free(schemas);
  astools_result result = {0};
  ASSERT_EQ_INT(astools_invoke(f.c, "mcp", "echo", "{msg:3}", 1000, &result), ASTOOLS_ERR_INVALID);
  ASSERT_TRUE(!marker(&f, "called"));
  astools_result_free(&result);
  ASSERT_EQ_INT(astools_invoke(f.c, "mcp", "unlisted", "{}", 1000, &result), ASTOOLS_ERR_NOT_FOUND);
  ASSERT_TRUE(!marker(&f, "called"));
  astools_result_free(&result);
  astools_tool *tool = NULL;
  ASSERT_OK(astools_registry_resolve(f.c, "mcp", &tool));
  ASSERT_TRUE(!tool->m->commands[0].read_only && !tool->m->commands[0].idempotent);
  char *rendered = NULL, *error = NULL;
  ASSERT_OK(astools_manifest_render(tool->m, &rendered));
  ASSERT_TRUE(rendered != NULL);
  astools_manifest *again = astools_manifest_parse(rendered, strlen(rendered), f.root, &error);
  ASSERT_TRUE(again != NULL && !error);
  ASSERT_EQ_STR(again->commands[0].mcp_name, "remote.echo");
  ASSERT_EQ_STR(again->commands[0].mcp_input_schema, input_schema);
  astools_manifest_free(again);
  free(error);
  free(rendered);
  astools_tool_unref(tool);
  drop(&f);
}
TEST(input_schema_is_enforced_even_when_local_parameter_types_allow_the_value) {
  fixture f;
  ASSERT_TRUE(setup(&f, "input-limit"));
  char *schemas = NULL;
  ASSERT_OK(astools_command_schemas(f.c, &schemas));
  ASSERT_TRUE(strstr(schemas, "allOf") && strstr(schemas, "maxLength"));
  free(schemas);
  astools_result result = {0};
  ASSERT_EQ_INT(call(&f, 1500, &result), ASTOOLS_ERR_INVALID);
  ASSERT_TRUE(!result.ok && !marker(&f, "called"));
  astools_result_free(&result);
  drop(&f);
}
TEST(structured_scalars_and_arrays_and_incomplete_results) {
  const char *modes[] = {"output-array", "output-null", "input-required"};
  for (size_t i = 0; i < 3; i++) {
    fixture f;
    ASSERT_TRUE(setup(&f, modes[i]));
    astools_result result = {0};
    ASSERT_OK(call(&f, 1500, &result));
    ASSERT_EQ_INT(result.ok, i != 2);
    jx_value *value = NULL;
    ASSERT_EQ_INT(jx_parse(result.result_xcdn, strlen(result.result_xcdn), &value), 0);
    if (i == 0) ASSERT_EQ_INT(jx_array_len(jx_object_get(value, "structuredContent")), 3);
    if (i == 1)
      ASSERT_TRUE(jx_object_get(value, "structuredContent") &&
                  jx_typeof(jx_object_get(value, "structuredContent")) == JX_NULL);
    if (i == 2) {
      ASSERT_EQ_STR(result.error_code, "astools/mcp-input-required");
      ASSERT_EQ_STR(astools_rpc_string(value, "requestState"), "opaque-state");
    }
    jx_free(value);
    astools_result_free(&result);
    drop(&f);
  }
}
TEST_LIST = {TEST_ENTRY(input_schema_is_enforced_even_when_local_parameter_types_allow_the_value),
             TEST_ENTRY(structured_scalars_and_arrays_and_incomplete_results),
             TEST_ENTRY(reuse_pagination_and_per_request_metadata),
             TEST_ENTRY(invalid_negotiation_or_binding_has_no_tool_effect),
             TEST_ENTRY(tool_errors_retain_content_and_corrupt_results_retire_the_peer),
             TEST_ENTRY(cancel_and_deadline_never_replay_an_uncertain_call),
             TEST_ENTRY(host_contract_owns_discovery_and_argument_admission)};
RUN_ALL_TESTS()
