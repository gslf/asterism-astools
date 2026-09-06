/* LSP-specific notifications use the common bounded JSON-RPC transport. */
#include "lsp.h"
#include "rpc.h"
typedef struct { const char *uri; int version; } observation;
static astools_err diagnostics(void *ud, const char *method, const jx_value *params, jx_value **out) {
  observation *want = ud;
  if (strcmp(method,"textDocument/publishDiagnostics")) return ASTOOLS_OK;
  const char *uri = astools_rpc_string(params,"uri");
  const jx_value *version = jx_object_get(params,"version"), *items = jx_object_get(params,"diagnostics");
  if (!uri || strcmp(uri,want->uri) || !jx_is_int(version) || jx_int_value(version) != want->version)
    return ASTOOLS_OK;
  if (jx_typeof(items) != JX_ARRAY) return ASTOOLS_ERR_PROTOCOL;
  *out = jx_clone(items); return *out ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
}
astools_err astools_lsp_receive(astools_ctx *c, astools_pproc *p, int id, const char *uri,
    int version, int64_t deadline, astools_task *cancel, jx_value **out) {
  observation want = {uri,version};
  return astools_rpc_receive(c,p,id,id ? NULL : diagnostics,&want,deadline,cancel,out);
}
