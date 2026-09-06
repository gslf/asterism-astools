/* Opt-in stdio MCP client. Local manifests own authority and model-facing schemas. */
#ifndef ASTOOLS_MCP_CLIENT_H
#define ASTOOLS_MCP_CLIENT_H
#include "rpc.h"
#define ASTOOLS_MCP_VERSION "2026-07-28"
astools_err astools_mcp_request(astools_ctx *c, astools_pproc *p, const char *method,
                                jx_value *params, int64_t deadline, astools_task *cancel,
                                jx_value **out);
bool astools_mcp_schema_equal(const jx_value *expected, const jx_value *observed);
jx_value *astools_mcp_arguments(const xcdn_node_t *node);
astools_err astools_mcp_initialize(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                   astools_task *cancel);
astools_err astools_mcp_check_tool(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                                   int64_t deadline, astools_task *cancel);
astools_err astools_mcp_invoke(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                               const xcdn_node_t *args, int64_t deadline, astools_task *cancel,
                               astools_result *result);
#endif
