/* Verify one locally allowed binding before each dispatch, with bounded pages. */
#include "mcp_client.h"
astools_err astools_mcp_check_tool(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                                   int64_t deadline, astools_task *cancel) {
  jx_value *input = NULL, *output = NULL;
  if (jx_parse(cmd->mcp_input_schema, strlen(cmd->mcp_input_schema), &input))
    return ASTOOLS_ERR_INVALID;
  if (cmd->mcp_output_schema &&
      jx_parse(cmd->mcp_output_schema, strlen(cmd->mcp_output_schema), &output)) {
    jx_free(input);
    return ASTOOLS_ERR_INVALID;
  }
  char cursor[513] = "";
  size_t count = 0, matches = 0;
  unsigned epoch = p->mcp_catalog_epoch;
  astools_err e = ASTOOLS_OK;
  for (unsigned page = 0; e == ASTOOLS_OK && page < 16; page++) {
    jx_value *params = jx_object(), *reply = NULL;
    if (!params || (*cursor && jx_object_set(params, "cursor", jx_string(cursor)))) {
      jx_free(params);
      e = ASTOOLS_ERR_NOMEM;
      break;
    }
    e = astools_mcp_request(c, p, "tools/list", params, deadline, cancel, &reply);
    if (e != ASTOOLS_OK) break;
    const jx_value *tools = jx_object_get(reply, "tools");
    const char *next = astools_rpc_string(reply, "nextCursor");
    size_t n = jx_array_len(tools);
    if (jx_typeof(tools) != JX_ARRAY || n > 1024 - count ||
        (jx_object_get(reply, "nextCursor") &&
         (!next || !*next || strlen(next) > 512 || !strcmp(cursor, next))))
      e = ASTOOLS_ERR_PROTOCOL;
    for (size_t i = 0; e == ASTOOLS_OK && i < n; i++) {
      const jx_value *tool = jx_array_at(tools, i);
      const char *name = astools_rpc_string(tool, "name");
      if (!name || !*name || strlen(name) > 128) {
        e = ASTOOLS_ERR_PROTOCOL;
        break;
      }
      if (strcmp(name, cmd->mcp_name)) continue;
      matches++;
      if (matches > 1 || !astools_mcp_schema_equal(input, jx_object_get(tool, "inputSchema")) ||
          !astools_mcp_schema_equal(output, jx_object_get(tool, "outputSchema")))
        e = ASTOOLS_ERR_PROTOCOL;
    }
    if (p->mcp_catalog_epoch != epoch) e = ASTOOLS_ERR_BUSY;
    count += n;
    bool done = !next;
    if (e == ASTOOLS_OK && next) strcpy(cursor, next);
    jx_free(reply);
    if (done) {
      if (e == ASTOOLS_OK && !matches) e = ASTOOLS_ERR_NOT_FOUND;
      break;
    }
    if (e == ASTOOLS_OK && page == 15) e = ASTOOLS_ERR_TOOL;
  }
  jx_free(input);
  jx_free(output);
  return e;
}
