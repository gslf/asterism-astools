/* Negotiate the capabilities actually used. No inferred UTF-16/UTF-8 offsets. */
#include "lsp.h"

#ifndef _WIN32
static int supported(const jx_value *caps, const char *name) {
  const jx_value *value = jx_object_get(caps, name);
  return (jx_typeof(value) == JX_BOOL && jx_bool_value(value)) || jx_typeof(value) == JX_OBJECT;
}
#endif

astools_err astools_lsp_initialize(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                   astools_task *cancel) {
#ifdef _WIN32
  (void)c;
  (void)p;
  (void)deadline;
  (void)cancel;
  return ASTOOLS_ERR_UNSUPPORTED;
#else
  static const char capabilities[] =
      "{\"general\":{\"positionEncodings\":[\"utf-8\"]},"
      "\"workspace\":{\"applyEdit\":false},\"textDocument\":{"
      "\"documentSymbol\":{\"hierarchicalDocumentSymbolSupport\":true},"
      "\"definition\":{\"linkSupport\":true},\"publishDiagnostics\":{\"versionSupport\":true}}}";
  jx_value *params = jx_object(), *caps = NULL, *options = jx_object(), *reply = NULL;
  char *uri = astools_lsp_uri(c->workspace);
  p->rpc_serial = 0;
  p->rpc_lines = false;
  p->lsp_capabilities = 0;
  p->lsp_provider[0] = 0;
  p->rpc_error[0] = 0;
  int bad = !params || !options || !uri || jx_parse(capabilities, sizeof capabilities - 1, &caps);
  bad |= jx_object_set(params, "processId", jx_null());
  bad |= jx_object_set(params, "rootUri", jx_string(uri ? uri : ""));
  bad |= jx_object_set(params, "capabilities", caps);
  /* Clangd's explicit database path avoids ancestor-directory discovery. */
  bad |= jx_object_set(options, "compilationDatabasePath", jx_string(c->workspace));
  bad |= jx_object_set(params, "initializationOptions", options);
  free(uri);
  if (bad) {
    jx_free(params);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_err e = astools_lsp_request(c, p, "initialize", params, deadline, cancel, &reply);
  if (e != ASTOOLS_OK) return e;
  const jx_value *reported = jx_object_get(reply, "capabilities"),
                 *sync = jx_object_get(reported, "textDocumentSync");
  const char *encoding = astools_lsp_string(reported, "positionEncoding");
  if (!encoding || strcmp(encoding, "utf-8") || !jx_bool_value(jx_object_get(sync, "openClose")))
    e = astools_seterr(
        c, ASTOOLS_ERR_UNSUPPORTED,
        "language server must negotiate UTF-8 positions and open/close synchronization");
  if (supported(reported, "documentSymbolProvider")) p->lsp_capabilities |= LSP_SYMBOLS;
  if (supported(reported, "definitionProvider")) p->lsp_capabilities |= LSP_DEFINITION;
  if (supported(reported, "referencesProvider")) p->lsp_capabilities |= LSP_REFERENCES;
  const jx_value *info = jx_object_get(reply, "serverInfo");
  const char *name = astools_lsp_string(info, "name"),
             *version = astools_lsp_string(info, "version");
  if (!name) name = "unknown";
  if (strlen(name) + (version ? strlen(version) + 2 : 0) >= sizeof p->lsp_provider)
    e = ASTOOLS_ERR_PROTOCOL;
  else
    snprintf(p->lsp_provider, sizeof p->lsp_provider, "%s%s%s", name, version ? ": " : "",
             version ? version : "");
  jx_free(reply);
  return e == ASTOOLS_OK ? astools_lsp_send(c, p, 0, "initialized", jx_object(), deadline, cancel)
                         : e;
#endif
}
