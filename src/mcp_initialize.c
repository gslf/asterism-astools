/* Probe the supported revision before any tool effect; never downgrade implicitly. */
#include "mcp_client.h"
astools_err astools_mcp_initialize(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                   astools_task *cancel) {
  p->rpc_lines = true;
  p->rpc_serial = 0;
  p->rpc_error[0] = 0;
  p->mcp_catalog_epoch = 0;
  jx_value *reply = NULL;
  astools_err e =
      astools_mcp_request(c, p, "server/discover", jx_object(), deadline, cancel, &reply);
  if (e != ASTOOLS_OK) return e;
  const jx_value *versions = jx_object_get(reply, "supportedVersions"),
                 *caps = jx_object_get(reply, "capabilities");
  bool supported = false;
  if (jx_typeof(versions) != JX_ARRAY || !jx_array_len(versions) || jx_array_len(versions) > 32)
    e = ASTOOLS_ERR_PROTOCOL;
  for (size_t i = 0; e == ASTOOLS_OK && i < jx_array_len(versions); i++) {
    const jx_value *v = jx_array_at(versions, i);
    const char *version = jx_string_value(v);
    if (!version || !*version || strlen(version) != jx_string_length(v) || strlen(version) > 64)
      e = ASTOOLS_ERR_PROTOCOL;
    else if (!strcmp(version, ASTOOLS_MCP_VERSION))
      supported = true;
  }
  if (e == ASTOOLS_OK && (!supported || jx_typeof(jx_object_get(caps, "tools")) != JX_OBJECT))
    e = ASTOOLS_ERR_UNSUPPORTED;
  if (e == ASTOOLS_ERR_UNSUPPORTED)
    snprintf(p->rpc_error, sizeof p->rpc_error, "Server must support MCP %s and tools",
             ASTOOLS_MCP_VERSION);
  jx_free(reply);
  return e;
}
