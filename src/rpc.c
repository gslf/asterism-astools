/* Strict JSON-RPC correlation; server requests can never authorize host effects. */
#include "rpc.h"

const char *astools_rpc_string(const jx_value *object, const char *key) {
  const jx_value *v = jx_object_get(object, key);
  const char *text = jx_string_value(v);
  return text && strlen(text) == jx_string_length(v) ? text : NULL;
}

astools_err astools_rpc_send(astools_ctx *c, astools_pproc *p, int id, const char *method,
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
  if (p->rpc_lines && strlen(text) > 1048576u) {
    free(text);
    return ASTOOLS_ERR_TOOL;
  }
  int why = 0;
  astools_err e = (p->rpc_lines ? astools_pp_write_all
                                : astools_pp_write_frame)(c, p, text, deadline, cancel, &why);
  free(text);
  return e;
}

static astools_err answer(astools_ctx *c, astools_pproc *p, const jx_value *id, bool ping,
                          int64_t deadline, astools_task *cancel) {
  jx_value *reply = jx_object(), *error = jx_object();
  int bad = !reply || !error;
  bad |= jx_object_set(reply, "jsonrpc", jx_string("2.0"));
  bad |= jx_object_set(reply, "id", jx_clone(id));
  if (!ping) {
    bad |= jx_object_set(error, "code", jx_int(-32601));
    bad |=
        jx_object_set(error, "message", jx_string("This client does not execute server requests"));
  }
  bad |= jx_object_set(reply, ping ? "result" : "error", error);
  char *text = bad ? NULL : jx_write(reply, 0);
  jx_free(reply);
  if (!text) return ASTOOLS_ERR_NOMEM;
  int why = 0;
  astools_err e = (p->rpc_lines ? astools_pp_write_all
                                : astools_pp_write_frame)(c, p, text, deadline, cancel, &why);
  free(text);
  return e;
}

/* An optional notification handler can complete an id-less observation. */
astools_err astools_rpc_receive(astools_ctx *c, astools_pproc *p, int id, astools_rpc_note note,
                                void *ud, int64_t deadline, astools_task *cancel, jx_value **out) {
  *out = NULL;
  for (unsigned messages = 0; messages < 256; messages++) {
    char *text = NULL;
    size_t len = 0;
    int why = 0;
    jx_value *message = NULL;
    astools_err e = (p->rpc_lines ? astools_pp_read_line : astools_pp_read_frame)(
        c, p, deadline, cancel, &text, &len, &why);
    if (e != ASTOOLS_OK) return e;
    if (p->rpc_lines && len > 1048576u) {
      free(text);
      return ASTOOLS_ERR_TOOL;
    }
    int bad = jx_parse(text, len, &message);
    free(text);
    const char *protocol = astools_rpc_string(message, "jsonrpc"),
               *method = astools_rpc_string(message, "method");
    if (bad || !protocol || strcmp(protocol, "2.0")) {
      jx_free(message);
      return ASTOOLS_ERR_PROTOCOL;
    }
    const jx_value *identity = jx_object_get(message, "id"),
                   *result = jx_object_get(message, "result"),
                   *error = jx_object_get(message, "error"),
                   *params = jx_object_get(message, "params");
    if (jx_object_get(message, "method")) {
      if (!method || result || error)
        e = ASTOOLS_ERR_PROTOCOL;
      else if (identity) {
        if (p->rpc_lines || (!jx_is_int(identity) && jx_typeof(identity) != JX_STRING))
          e = ASTOOLS_ERR_PROTOCOL;
        else
          e = answer(c, p, identity, !strcmp(method, "ping"), deadline, cancel);
      } else {
        if (p->rpc_lines && !strcmp(method, "notifications/tools/list_changed"))
          p->mcp_catalog_epoch++;
        if (note) e = note(ud, method, params, out);
      }

      jx_free(message);
      if (e != ASTOOLS_OK || *out) return e;
      continue;
    }
    if (!id || !jx_is_int(identity) || jx_int_value(identity) != id || (!result == !error))
      e = ASTOOLS_ERR_PROTOCOL;
    else if (error) {
      const jx_value *code = jx_object_get(error, "code");
      const char *detail = astools_rpc_string(error, "message");
      if (!jx_is_int(code) || !detail)
        e = ASTOOLS_ERR_PROTOCOL;
      else {
        long long value = jx_int_value(code);
        e = p->rpc_lines
                ? ((value == -32021 || value == -32022 || value == -32601) ? ASTOOLS_ERR_UNSUPPORTED
                                                                           : ASTOOLS_ERR_TOOL)
            : value == -32800 ? ASTOOLS_ERR_CANCELLED
            : value == -32801 ? ASTOOLS_ERR_BUSY
            : value == -32601 ? ASTOOLS_ERR_UNSUPPORTED
                              : ASTOOLS_ERR_TOOL;
        int prefix = snprintf(p->rpc_error, sizeof p->rpc_error, "Server error %lld: ", value);
        size_t n = strlen(detail), cap = sizeof p->rpc_error - (size_t)prefix - 1;
        if (n > cap) n = cap;
        while (n && ((unsigned char)detail[n] & 0xc0) == 0x80) n--;
        memcpy(p->rpc_error + prefix, detail, n);
        p->rpc_error[(size_t)prefix + n] = 0;
      }
    } else if (!(*out = jx_clone(result)))
      e = ASTOOLS_ERR_NOMEM;
    jx_free(message);
    return e;
  }
  return astools_seterr(c, ASTOOLS_ERR_TOOL, "server exceeded the notification budget");
}

astools_err astools_rpc_request(astools_ctx *c, astools_pproc *p, const char *method,
                                jx_value *params, int64_t deadline, astools_task *cancel,
                                jx_value **out) {
  *out = NULL;
  if (p->rpc_serial == INT32_MAX) {
    jx_free(params);
    return ASTOOLS_ERR_TOOL;
  }
  int id = ++p->rpc_serial;
  astools_err e = astools_rpc_send(c, p, id, method, params, deadline, cancel);
  if (e == ASTOOLS_OK) e = astools_rpc_receive(c, p, id, NULL, NULL, deadline, cancel, out);
  if (p->rpc_lines && (e == ASTOOLS_ERR_TIMEOUT || e == ASTOOLS_ERR_CANCELLED)) {
    jx_value *stopped = jx_object();
    if (stopped && !jx_object_set(stopped, "requestId", jx_int(id)))
      (void)astools_rpc_send(c, p, 0, "notifications/cancelled", stopped, astools_mono(c) + 50,
                             NULL);
    else
      jx_free(stopped);
  }
  return e;
}
