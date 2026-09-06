/* Normalize server locations against readable, versioned workspace files. */
#include "lsp_results.h"

static astools_err remaining(const astools_lsp_view *v) {
  if (astools_task_cancelled(v->cancel)) return ASTOOLS_ERR_CANCELLED;
  return v->deadline && astools_mono(v->ctx) >= v->deadline ? ASTOOLS_ERR_TIMEOUT : ASTOOLS_OK;
}

static astools_err target(astools_lsp_view *v, const char *uri, astools_lsp_file **out) {
  *out = NULL;
  char *path = astools_lsp_path(uri);
  if (!path) return ASTOOLS_ERR_PROTOCOL;
  for (size_t i = 0; i < v->count; i++)
    if (!strcmp(path, v->files[i].path)) {
      *out = &v->files[i];
      free(path);
      return ASTOOLS_OK;
    }
  if (v->count == LSP_FILES) {
    free(path);
    return ASTOOLS_ERR_TOOL;
  }
  astools_err e = astools_lsp_file_read(v->ctx->workspace, path, v->grants, &v->files[v->count]);
  free(path);
  if (e == ASTOOLS_ERR_DENIED) {
    v->excluded++;
    return ASTOOLS_OK;
  }
  if (e != ASTOOLS_OK) return e;
  *out = &v->files[v->count++];
  return ASTOOLS_OK;
}

static astools_err range(const astools_lsp_file *file, const jx_value *input, jx_value **out) {
  size_t start, end;
  *out = NULL;
  const jx_value *a = jx_object_get(input, "start"), *b = jx_object_get(input, "end");
  if (!astools_lsp_offset(file, a, &start) || !astools_lsp_offset(file, b, &end) || end < start)
    return ASTOOLS_ERR_PROTOCOL;
  jx_value *result = jx_object();
  int bad = !result;
  bad |= jx_object_set(result, "start_byte", jx_int((long long)start));
  bad |= jx_object_set(result, "end_byte", jx_int((long long)end));
  bad |= jx_object_set(result, "start_line", jx_int(jx_int_value(jx_object_get(a, "line")) + 1));
  bad |= jx_object_set(result, "start_column",
                       jx_int(jx_int_value(jx_object_get(a, "character")) + 1));
  bad |= jx_object_set(result, "end_line", jx_int(jx_int_value(jx_object_get(b, "line")) + 1));
  bad |=
      jx_object_set(result, "end_column", jx_int(jx_int_value(jx_object_get(b, "character")) + 1));
  if (bad) {
    jx_free(result);
    return ASTOOLS_ERR_NOMEM;
  }
  *out = result;
  return ASTOOLS_OK;
}

static astools_err item(astools_lsp_view *v, const jx_value *input, int symbol, int diagnostic) {
  const char *uri = astools_lsp_string(input, "uri");
  const jx_value *span = jx_object_get(input, "range"),
                 *selection = jx_object_get(input, "selectionRange");
  if (!uri && jx_object_get(input, "targetUri")) {
    uri = astools_lsp_string(input, "targetUri");
    span = jx_object_get(input, "targetRange");
    selection = jx_object_get(input, "targetSelectionRange");
  }
  if (symbol && jx_object_get(input, "location")) {
    const jx_value *location = jx_object_get(input, "location");
    uri = astools_lsp_string(location, "uri");
    if (!uri) return ASTOOLS_ERR_PROTOCOL;
    span = jx_object_get(location, "range");
  }
  astools_lsp_file *file = &v->files[0];
  astools_err e = ASTOOLS_OK;
  if (uri) e = target(v, uri, &file);
  else if (!symbol && !diagnostic) e = ASTOOLS_ERR_PROTOCOL;
  if (e != ASTOOLS_OK || !file) return e;
  jx_value *row = jx_object(), *location = NULL, *selected = NULL;
  if (!row) return ASTOOLS_ERR_NOMEM;
  e = range(file, span, &location);
  if (e == ASTOOLS_OK && selection) e = range(file, selection, &selected);
  if (e == ASTOOLS_OK && selected &&
      (jx_int_value(jx_object_get(selected, "start_byte")) <
           jx_int_value(jx_object_get(location, "start_byte")) ||
       jx_int_value(jx_object_get(selected, "end_byte")) >
           jx_int_value(jx_object_get(location, "end_byte"))))
    e = ASTOOLS_ERR_PROTOCOL;
  int bad = 0;
  if (e == ASTOOLS_OK) {
    const char *relative = file->path + strlen(v->ctx->workspace);
    if (*relative == '/') relative++;
    bad |= jx_object_set(row, "path", jx_string(relative));
    bad |= jx_object_set(row, "sha256", jx_string(file->sha256));
    bad |= jx_object_set(row, "range", location);
    location = NULL;
    if (selected) {
      bad |= jx_object_set(row, "selection", selected);
      selected = NULL;
    }
    if (symbol) {
      const char *name = astools_lsp_string(input, "name");
      const jx_value *kind = jx_object_get(input, "kind");
      if (!name || strlen(name) > 1024 || !jx_is_int(kind) || jx_int_value(kind) < 1 ||
          jx_int_value(kind) > 26)
        e = ASTOOLS_ERR_PROTOCOL;
      else {
        bad |= jx_object_set(row, "name", jx_string(name));
        bad |= jx_object_set(row, "kind", jx_clone(kind));
      }
    }
    if (diagnostic) {
      const char *message = astools_lsp_string(input, "message");
      const jx_value *severity = jx_object_get(input, "severity");
      if (!message || strlen(message) > 8192 ||
          (severity &&
           (!jx_is_int(severity) || jx_int_value(severity) < 1 || jx_int_value(severity) > 4)))
        e = ASTOOLS_ERR_PROTOCOL;
      else {
        bad |= jx_object_set(row, "message", jx_string(message));
        if (severity) bad |= jx_object_set(row, "severity", jx_clone(severity));
      }
    }
  }
  jx_free(location);
  jx_free(selected);
  if (bad) e = ASTOOLS_ERR_NOMEM;
  if (e == ASTOOLS_OK) return jx_array_push(v->items, row) ? ASTOOLS_ERR_NOMEM : ASTOOLS_OK;
  jx_free(row);
  return e;
}

static astools_err rows(astools_lsp_view *v, const jx_value *array, int symbol, int diagnostic,
                        unsigned depth) {
  if (depth > 32 || jx_typeof(array) != JX_ARRAY) return ASTOOLS_ERR_PROTOCOL;
  for (size_t i = 0; i < jx_array_len(array); i++) {
    astools_err budget = remaining(v);
    if (budget != ASTOOLS_OK) return budget;
    if (jx_array_len(v->items) == LSP_RESULTS) {
      v->truncated = 1;
      break;
    }
    const jx_value *value = jx_array_at(array, i), *children = jx_object_get(value, "children");
    astools_err e = item(v, value, symbol, diagnostic);
    if (e == ASTOOLS_OK && symbol && children) e = rows(v, children, symbol, diagnostic, depth + 1);
    if (e != ASTOOLS_OK) return e;
  }
  return ASTOOLS_OK;
}

astools_err astools_lsp_render(astools_lsp_view *v, const char *command, const jx_value *reply) {
  v->items = jx_array();
  if (!v->items) return ASTOOLS_ERR_NOMEM;
  if (jx_typeof(reply) == JX_NULL) return ASTOOLS_OK;
  int symbol = !strcmp(command, "symbols"), diagnostic = !strcmp(command, "diagnostics");
  if (!symbol && !diagnostic && jx_typeof(reply) == JX_OBJECT) return item(v, reply, 0, 0);
  return rows(v, reply, symbol, diagnostic, 0);
}

astools_err astools_lsp_recheck(astools_lsp_view *view) {
  for (size_t i = 0; i < view->count; i++) {
    astools_err budget = remaining(view);
    if (budget != ASTOOLS_OK) return budget;
    astools_lsp_file current;
    astools_err e =
        astools_lsp_file_read(view->ctx->workspace, view->files[i].path, view->grants, &current);
    if (e == ASTOOLS_OK && strcmp(current.sha256, view->files[i].sha256)) e = ASTOOLS_ERR_BUSY;
    astools_lsp_file_free(&current);
    if (e != ASTOOLS_OK) return e;
  }
  return ASTOOLS_OK;
}

void astools_lsp_view_free(astools_lsp_view *view) {
  for (size_t i = 0; i < view->count; i++) astools_lsp_file_free(&view->files[i]);
  jx_free(view->items);
}
