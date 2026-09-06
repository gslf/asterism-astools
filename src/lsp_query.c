/* One exclusive query opens exact content, observes results, then closes it. */
#include "lsp_results.h"

static const char *arg(const xcdn_node_t *args, const char *key) {
  const xcdn_node_t *value = xcdn_object_get(args->value, key);
  return value && value->value && value->value->type == XCDN_VAL_STRING ? value->value->data.string
                                                                        : NULL;
}
static long long number(const xcdn_node_t *args, const char *key) {
  const xcdn_node_t *value = xcdn_object_get(args->value, key);
  return value && value->value && value->value->type == XCDN_VAL_INT ? value->value->data.integer
                                                                     : -1;
}
static jx_value *document(const astools_lsp_file *file) {
  jx_value *params = jx_object(), *doc = jx_object();
  int bad = !params || !doc;
  bad |= jx_object_set(doc, "uri", jx_string(file->uri));
  bad |= jx_object_set(params, "textDocument", doc);
  if (bad) {
    jx_free(params);
    return NULL;
  }
  return params;
}

static astools_err query(astools_ctx *c, astools_pproc *p, const char *command,
                         const xcdn_node_t *args, astools_lsp_file *file, int64_t deadline,
                         astools_task *cancel, jx_value **out) {
  const char *method = !strcmp(command, "symbols")      ? "textDocument/documentSymbol"
                       : !strcmp(command, "definition") ? "textDocument/definition"
                                                        : "textDocument/references";
  unsigned capability = !strcmp(command, "symbols")      ? LSP_SYMBOLS
                        : !strcmp(command, "definition") ? LSP_DEFINITION
                                                         : LSP_REFERENCES;
  int diagnostics = !strcmp(command, "diagnostics"),
      positional = !strcmp(command, "definition") || !strcmp(command, "references");
  if (!diagnostics && !(p->lsp_capabilities & capability)) return ASTOOLS_ERR_UNSUPPORTED;
  if (p->lsp_serial >= INT32_MAX - 1) return ASTOOLS_ERR_TOOL;
  const char *dot = strrchr(file->path, '.');
  if (!dot ||
      (strcmp(dot, ".c") && strcmp(dot, ".h") && strcmp(dot, ".cc") && strcmp(dot, ".cpp") &&
       strcmp(dot, ".cxx") && strcmp(dot, ".hpp") && strcmp(dot, ".hh") && strcmp(dot, ".hxx")))
    return ASTOOLS_ERR_UNSUPPORTED;
  long long line = number(args, "line"), column = number(args, "column");
  if (positional && (line < 1 || line > INT32_MAX || column < 1 || column > INT32_MAX))
    return ASTOOLS_ERR_INVALID;
  jx_value *params = document(file), *request = document(file);
  if (!params || !request) {
    jx_free(params);
    jx_free(request);
    return ASTOOLS_ERR_NOMEM;
  }
  jx_value *doc = jx_object_get(params, "textDocument");
  int version = ++p->lsp_serial;
  int bad = jx_object_set(doc, "version", jx_int(version));
  bad |= jx_object_set(doc, "languageId", jx_string(!strcmp(dot, ".c") ? "c" : "cpp"));
  bad |= jx_object_set(doc, "text", jx_string(file->text));
  if (positional) {
    jx_value *position = jx_object();
    bad |= jx_object_set(position, "line", jx_int(line - 1));
    bad |= jx_object_set(position, "character", jx_int(column - 1));
    size_t offset = 0;
    if (!astools_lsp_offset(file, position, &offset)) {
      jx_free(position);
      jx_free(params);
      jx_free(request);
      return ASTOOLS_ERR_INVALID;
    }
    bad |= jx_object_set(request, "position", position);
    if (!strcmp(command, "references")) {
      jx_value *context = jx_object();
      bad |= jx_object_set(context, "includeDeclaration", jx_bool(1));
      bad |= jx_object_set(request, "context", context);
    }
  }
  if (bad) {
    jx_free(params);
    jx_free(request);
    return ASTOOLS_ERR_NOMEM;
  }
  astools_err e = astools_lsp_send(c, p, 0, "textDocument/didOpen", params, deadline, cancel);
  if (e == ASTOOLS_OK) {
    if (diagnostics) e = astools_lsp_receive(c, p, 0, file->uri, version, deadline, cancel, out);
    else {
      e = astools_lsp_request(c, p, method, request, deadline, cancel, out);
      request = NULL;
    }
  }
  jx_free(request);
  if (e == ASTOOLS_OK)
    e = astools_lsp_send(c, p, 0, "textDocument/didClose", document(file), deadline, cancel);
  return e;
}

astools_err astools_lsp_invoke(astools_ctx *c, astools_pproc *p, const astools_cmd *cmd,
                               const xcdn_node_t *args, const astools_effective *grants,
                               int64_t deadline, astools_task *cancel, astools_result *result) {
  astools_lsp_view view = {0};
  p->lsp_error[0] = 0;
  jx_value *reply = NULL, *output = NULL;
  view.ctx = c;
  view.grants = grants;
  view.deadline = deadline;
  view.cancel = cancel;
  const char *path = arg(args, "path"), *expected = arg(args, "sha256");
  if (!path || !expected || strlen(expected) != 64 ||
      (strcmp(cmd->name, "symbols") && strcmp(cmd->name, "definition") &&
       strcmp(cmd->name, "references") && strcmp(cmd->name, "diagnostics")))
    return ASTOOLS_ERR_INVALID;
  if (strspn(expected, "0123456789abcdef") != 64) return ASTOOLS_ERR_INVALID;
  astools_err e = astools_lsp_file_read(c->workspace, path, grants, &view.files[0]);
  if (e != ASTOOLS_OK) return e;
  view.count = 1;
  if (strcmp(expected, view.files[0].sha256)) {
    e = ASTOOLS_ERR_BUSY;
    goto done;
  }
  e = query(c, p, cmd->name, args, &view.files[0], deadline, cancel, &reply);
  if (e == ASTOOLS_OK) e = astools_lsp_render(&view, cmd->name, reply);
  if (e == ASTOOLS_OK) e = astools_lsp_recheck(&view);
  if (e == ASTOOLS_OK && astools_task_cancelled(cancel)) e = ASTOOLS_ERR_CANCELLED;
  if (e == ASTOOLS_OK && astools_mono(c) >= deadline) e = ASTOOLS_ERR_TIMEOUT;
  if (e != ASTOOLS_OK) goto done;
  output = jx_object();
  int bad = !output;
  bad |= jx_object_set(output, "provider", jx_string(p->lsp_provider));
  bad |= jx_object_set(output, "position_encoding", jx_string("utf-8"));
  bad |= jx_object_set(output, "source_sha256", jx_string(view.files[0].sha256));
  bad |= jx_object_set(output, "coverage",
                       jx_string("server locations with current host file hashes; dependency "
                                 "freshness and index coverage are not certified"));
  bad |= jx_object_set(output, "truncated", jx_bool(view.truncated));
  bad |= jx_object_set(output, "excluded", jx_int((long long)view.excluded));
  bad |= jx_object_set(output, "items", view.items);
  view.items = NULL;
  if (bad || !(result->result_xcdn = jx_write(output, 0))) e = ASTOOLS_ERR_NOMEM;
  else if (c->cfg.max_output_bytes > 0 &&
           strlen(result->result_xcdn) > (size_t)c->cfg.max_output_bytes) {
    free(result->result_xcdn);
    result->result_xcdn = NULL;
    e = ASTOOLS_ERR_TOOL;
  } else result->ok = true;
done:
  jx_free(output);
  jx_free(reply);
  astools_lsp_view_free(&view);
  return e;
}
