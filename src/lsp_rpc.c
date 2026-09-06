/* Strict JSON-RPC correlation; server requests can never authorize host effects. */
#include "lsp.h"

const char *astools_lsp_string(const jx_value *object, const char *key) {
  const jx_value *v = jx_object_get(object, key);
  const char *text = jx_string_value(v);
  return text && strlen(text) == jx_string_length(v) ? text : NULL;
}

astools_err astools_lsp_send(astools_ctx *c, astools_pproc *p, int id, const char *method,
                             jx_value *params, int64_t deadline, astools_task *cancel) {
  jx_value *message = jx_object();
  int bad = !message;
  bad |= jx_object_set(message, "jsonrpc", jx_string("2.0"));
  if (id) bad |= jx_object_set(message, "id", jx_int(id));
  bad |= jx_object_set(message, "method", jx_string(method));
  bad |= jx_object_set(message, "params", params);
  char *text = bad ? NULL : jx_write(message, 0);
  jx_free(message);
  if (!text) return ASTOOLS_ERR_NOMEM;
  int why = 0;
  astools_err e = astools_pp_write_frame(c, p, text, deadline, cancel, &why);
  free(text);
  return e;
}

static astools_err refuse(astools_ctx *c, astools_pproc *p, const jx_value *id, int64_t deadline,
                          astools_task *cancel) {
  jx_value *reply = jx_object(), *error = jx_object();
  int bad = !reply || !error;
  bad |= jx_object_set(reply, "jsonrpc", jx_string("2.0"));
  bad |= jx_object_set(reply, "id", jx_clone(id));
  bad |= jx_object_set(error, "code", jx_int(-32601));
  bad |= jx_object_set(error, "message", jx_string("This client does not execute server requests"));
  bad |= jx_object_set(reply, "error", error);
  char *text = bad ? NULL : jx_write(reply, 0);
  jx_free(reply);
  if (!text) return ASTOOLS_ERR_NOMEM;
  int why = 0;
  astools_err e = astools_pp_write_frame(c, p, text, deadline, cancel, &why);
  free(text);
  return e;
}

/* id==0 waits for diagnostics of one exact open-document revision. */
astools_err astools_lsp_receive(astools_ctx *c, astools_pproc *p, int id, const char *uri,
                                int version, int64_t deadline, astools_task *cancel,
                                jx_value **out) {
  *out = NULL;
  for (unsigned messages = 0; messages < 256; messages++) {
    char *text = NULL;
    size_t len = 0;
    int why = 0;
    jx_value *message = NULL;
    astools_err e = astools_pp_read_frame(c, p, deadline, cancel, &text, &len, &why);
    if (e != ASTOOLS_OK) return e;
    int bad = jx_parse(text, len, &message);
    free(text);
    const char *protocol = astools_lsp_string(message, "jsonrpc"),
               *method = astools_lsp_string(message, "method");
    if (bad || !protocol || strcmp(protocol, "2.0")) {
      jx_free(message);
      return ASTOOLS_ERR_PROTOCOL;
    }
    const jx_value *identity = jx_object_get(message, "id"),
                   *result = jx_object_get(message, "result"),
                   *error = jx_object_get(message, "error"),
                   *params = jx_object_get(message, "params");
    if (jx_object_get(message, "method")) {
      if (!method || result || error) e = ASTOOLS_ERR_PROTOCOL;
      else if (identity) {
        if (!jx_is_int(identity) && jx_typeof(identity) != JX_STRING) e = ASTOOLS_ERR_PROTOCOL;
        else e = refuse(c, p, identity, deadline, cancel);
      } else if (!id && !strcmp(method, "textDocument/publishDiagnostics")) {
        const char *doc = astools_lsp_string(params, "uri");
        const jx_value *revision = jx_object_get(params, "version"),
                       *diagnostics = jx_object_get(params, "diagnostics");
        if (doc && !strcmp(doc, uri) && jx_is_int(revision) && jx_int_value(revision) == version) {
          if (jx_typeof(diagnostics) != JX_ARRAY) e = ASTOOLS_ERR_PROTOCOL;
          else if (!(*out = jx_clone(diagnostics))) e = ASTOOLS_ERR_NOMEM;
        }
      }
      jx_free(message);
      if (e != ASTOOLS_OK || *out) return e;
      continue;
    }
    if (!id || !jx_is_int(identity) || jx_int_value(identity) != id || (!result == !error))
      e = ASTOOLS_ERR_PROTOCOL;
    else if (error) {
      const jx_value *code = jx_object_get(error, "code");
      const char *detail = astools_lsp_string(error, "message");
      if (!jx_is_int(code) || !detail) e = ASTOOLS_ERR_PROTOCOL;
      else {
        long long value = jx_int_value(code);
        e = value == -32800   ? ASTOOLS_ERR_CANCELLED
            : value == -32801 ? ASTOOLS_ERR_BUSY
            : value == -32601 ? ASTOOLS_ERR_UNSUPPORTED
                              : ASTOOLS_ERR_TOOL;
        int prefix = snprintf(p->lsp_error, sizeof p->lsp_error, "Server error %lld: ", value);
        size_t n = strlen(detail), cap = sizeof p->lsp_error - (size_t)prefix - 1;
        if (n > cap) n = cap;
        while (n && ((unsigned char)detail[n] & 0xc0) == 0x80) n--;
        memcpy(p->lsp_error + prefix, detail, n);
        p->lsp_error[(size_t)prefix + n] = 0;
      }
    } else if (!(*out = jx_clone(result))) e = ASTOOLS_ERR_NOMEM;
    jx_free(message);
    return e;
  }
  return astools_seterr(c, ASTOOLS_ERR_TOOL, "language server exceeded the notification budget");
}

astools_err astools_lsp_request(astools_ctx *c, astools_pproc *p, const char *method,
                                jx_value *params, int64_t deadline, astools_task *cancel,
                                jx_value **out) {
  *out = NULL;
  if (p->lsp_serial == INT32_MAX) {
    jx_free(params);
    return ASTOOLS_ERR_TOOL;
  }
  int id = ++p->lsp_serial;
  astools_err e = astools_lsp_send(c, p, id, method, params, deadline, cancel);
  return e == ASTOOLS_OK ? astools_lsp_receive(c, p, id, NULL, 0, deadline, cancel, out) : e;
}
