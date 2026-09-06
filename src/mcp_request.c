/* Every request carries its own identity and capabilities; transport is not a session. */
#include "mcp_client.h"
astools_err astools_mcp_request(astools_ctx *c, astools_pproc *p, const char *method,
                                jx_value *params, int64_t deadline, astools_task *cancel,
                                jx_value **out) {
  *out = NULL;
  jx_value *meta = jx_object(), *info = jx_object();
  int bad = !params || !meta || !info;
  bad |= jx_object_set(info, "name", jx_string("astools"));
  bad |= jx_object_set(info, "version", jx_string(ASTOOLS_VERSION));
  bad |= jx_object_set(meta, "io.modelcontextprotocol/clientInfo", info);
  bad |= jx_object_set(meta, "io.modelcontextprotocol/clientCapabilities", jx_object());
  bad |= jx_object_set(meta, "io.modelcontextprotocol/protocolVersion",
                       jx_string(ASTOOLS_MCP_VERSION));
  bad |= jx_object_set(params, "_meta", meta);
  if (bad) {
    jx_free(params);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_err e = astools_rpc_request(c, p, method, params, deadline, cancel, out);
  if (e != ASTOOLS_OK) return e;
  const char *type = astools_rpc_string(*out, "resultType");
  const jx_value *metadata = jx_object_get(*out, "_meta");
  /* The base protocol defines omitted resultType as complete. No handshake fallback. */
  if (jx_typeof(*out) != JX_OBJECT || (metadata && jx_typeof(metadata) != JX_OBJECT) ||
      (jx_object_get(*out, "resultType") &&
       (!type || (strcmp(type, "complete") &&
                  (strcmp(type, "input_required") || strcmp(method, "tools/call")))))) {
    jx_free(*out);
    *out = NULL;
    return ASTOOLS_ERR_PROTOCOL;
  }
  return ASTOOLS_OK;
}
