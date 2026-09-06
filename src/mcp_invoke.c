/* Preserve the MCP result envelope; references are data, never automatic reads. */
#include "mcp_client.h"
#include "mcp_schema.h"
static bool content_valid(const jx_value *item) {
  const char *type = astools_rpc_string(item, "type");
  if (!type) return false;
  if (!strcmp(type, "text")) return astools_rpc_string(item, "text") != NULL;
  if (!strcmp(type, "image") || !strcmp(type, "audio"))
    return astools_rpc_string(item, "data") && astools_rpc_string(item, "mimeType");
  if (!strcmp(type, "resource_link"))
    return astools_rpc_string(item, "uri") && astools_rpc_string(item, "name");
  if (!strcmp(type, "resource")) {
    const jx_value *resource = jx_object_get(item, "resource");
    return astools_rpc_string(resource, "uri") && ((astools_rpc_string(resource, "text") != NULL) !=
                                                   (astools_rpc_string(resource, "blob") != NULL));
  }
  return false;
}
astools_err astools_mcp_invoke(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                               const xcdn_node_t *args, int64_t deadline, astools_task *cancel,
                               astools_result *result) {
  p->rpc_error[0] = 0;
  jx_value *arguments = astools_mcp_arguments(args);
  if (!arguments) return ASTOOLS_ERR_INVALID;
  jx_value *input = NULL;
  if (jx_parse(cmd->mcp_input_schema, strlen(cmd->mcp_input_schema), &input) ||
      !astools_mcp_schema_validate(input, arguments)) {
    jx_free(input);
    jx_free(arguments);
    snprintf(p->rpc_error, sizeof p->rpc_error, "Arguments violate the reviewed MCP input schema");
    return ASTOOLS_ERR_INVALID;
  }
  jx_free(input);
  astools_err e = astools_mcp_check_tool(c, p, cmd, deadline, cancel);
  if (e != ASTOOLS_OK) {
    jx_free(arguments);
    return e;
  }
  jx_value *params = jx_object(), *reply = NULL;
  int bad = !params;
  bad |= jx_object_set(params, "name", jx_string(cmd->mcp_name));
  bad |= jx_object_set(params, "arguments", arguments);
  if (bad) {
    jx_free(params);
    return ASTOOLS_ERR_NOMEM;
  }
  e = astools_mcp_request(c, p, "tools/call", params, deadline, cancel, &reply);
  if (e != ASTOOLS_OK) return e;
  const char *type = astools_rpc_string(reply, "resultType");
  bool incomplete = type && !strcmp(type, "input_required");
  const jx_value *content = jx_object_get(reply, "content"),
                 *failed = jx_object_get(reply, "isError"),
                 *structured = jx_object_get(reply, "structuredContent");
  for (size_t i = 0; i < jx_object_count(reply); i++) {
    const char *key = jx_object_key_at(reply, i);
    bool common = !strcmp(key, "resultType") || !strcmp(key, "_meta");
    bool allowed = incomplete ? (!strcmp(key, "inputRequests") || !strcmp(key, "requestState"))
                              : (!strcmp(key, "content") || !strcmp(key, "isError") ||
                                 !strcmp(key, "structuredContent"));
    if (!common && !allowed) e = ASTOOLS_ERR_PROTOCOL;
  }
  if (incomplete) {
    const jx_value *requests = jx_object_get(reply, "inputRequests"),
                   *state = jx_object_get(reply, "requestState");
    /* No client capabilities were advertised. Preserve state without inspecting or replaying it. */
    if ((!requests && !state) ||
        (requests && (jx_typeof(requests) != JX_OBJECT || jx_object_count(requests))) ||
        (state && !astools_rpc_string(reply, "requestState")))
      e = ASTOOLS_ERR_PROTOCOL;
  } else {
    if (jx_typeof(content) != JX_ARRAY || jx_array_len(content) > 128 ||
        (failed && jx_typeof(failed) != JX_BOOL) ||
        (cmd->mcp_output_schema && !jx_bool_value(failed) && !structured))
      e = ASTOOLS_ERR_PROTOCOL;
    for (size_t i = 0; e == ASTOOLS_OK && i < jx_array_len(content); i++)
      if (!content_valid(jx_array_at(content, i))) e = ASTOOLS_ERR_PROTOCOL;
    if (e == ASTOOLS_OK && structured && cmd->mcp_output_schema) {
      jx_value *output = NULL;
      if (jx_parse(cmd->mcp_output_schema, strlen(cmd->mcp_output_schema), &output) ||
          !astools_mcp_schema_validate(output, structured)) {
        e = ASTOOLS_ERR_PROTOCOL;
        snprintf(p->rpc_error, sizeof p->rpc_error,
                 "Result violates the reviewed MCP output schema");
      }
      jx_free(output);
    }
  }
  if (e == ASTOOLS_OK && astools_task_cancelled(cancel)) e = ASTOOLS_ERR_CANCELLED;
  if (e == ASTOOLS_OK && astools_mono(c) >= deadline) e = ASTOOLS_ERR_TIMEOUT;
  if (e == ASTOOLS_OK) {
    result->result_xcdn = jx_write(reply, 0);
    if (!result->result_xcdn)
      e = ASTOOLS_ERR_NOMEM;
    else if (c->cfg.max_output_bytes > 0 &&
             strlen(result->result_xcdn) > (size_t)c->cfg.max_output_bytes)
      e = ASTOOLS_ERR_TOOL;
    else {
      result->ok = !incomplete && !jx_bool_value(failed);
      if (!result->ok) {
        result->error_code =
            astools_strdup(incomplete ? "astools/mcp-input-required" : "astools/mcp-tool");
        result->error_message = astools_strdup(
            incomplete ? "MCP request is incomplete; no automatic continuation was executed"
                       : "MCP tool reported an execution error; see the complete result");
        if (!result->error_code || !result->error_message) e = ASTOOLS_ERR_NOMEM;
      }
    }
  }
  jx_free(reply);
  return e;
}
