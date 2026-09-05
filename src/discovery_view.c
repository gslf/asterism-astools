/* A selection owns schemas and pins immutable manifest descriptors. */
#include "discovery.h"
#include <stdlib.h>
#include <string.h>
void astools_selection_free(astools_selection *s) {
  if (!s) return;
  for (size_t i = 0; i < s->count; i++) {
    astools_selected_command *v = &s->commands[i];
    free((char *)v->tool);
    free((char *)v->ref);
    free((char *)v->arguments);
    free((char *)v->summary);
    free((char *)v->content_sha256);
    astools_tool_unref(s->views[i].tool);
  }
  free(s->catalog);
  free(s->grammar);
  free(s->schemas);
  free(s);
}
size_t astools_selection_count(const astools_selection *s) { return s ? s->count : 0; }
size_t astools_selection_omitted(const astools_selection *s) { return s ? s->omitted : 0; }
uint64_t astools_selection_revision(const astools_selection *s) { return s ? s->revision : 0; }
const astools_selected_command *astools_selection_get(const astools_selection *s, size_t i) {
  return s && i < s->count ? &s->commands[i] : NULL;
}
const char *astools_selection_catalog(const astools_selection *s) { return s ? s->catalog : NULL; }
const char *astools_selection_grammar(const astools_selection *s) { return s ? s->grammar : NULL; }
const char *astools_selection_schemas(const astools_selection *s) { return s ? s->schemas : NULL; }
static size_t find(const astools_selection *s, const char *tool) {
  if (s && tool)
    for (size_t i = 0; i < s->count; i++)
      if (!strcmp(s->commands[i].tool, tool)) return i;
  return ASTOOLS_SELECTION_MAX;
}
astools_err astools_selection_invoke(astools_selection *s, const char *tool, const char *args,
                                     uint32_t deadline_ms, astools_result *out) {
  if (out) memset(out, 0, sizeof *out);
  size_t i = find(s, tool);
  if (i == ASTOOLS_SELECTION_MAX) return ASTOOLS_ERR_DENIED;
  const astools_selected_command *v = &s->commands[i];
  return astools_invoke_impl(s->ctx, v->ref, v->command, args, deadline_ms, NULL,
                             s->views[i].tool->content_sha256, out);
}
astools_err astools_selection_invoke_async(astools_selection *s, const char *tool, const char *args,
                                           uint32_t deadline_ms, astools_task **out) {
  if (out) *out = NULL;
  size_t i = find(s, tool);
  if (i == ASTOOLS_SELECTION_MAX) return ASTOOLS_ERR_DENIED;
  const astools_selected_command *v = &s->commands[i];
  return astools_invoke_async_checked(s->ctx, v->ref, v->command, args, deadline_ms,
                                      s->views[i].tool->content_sha256, out);
}

astools_err astools_selection_validate(astools_selection *s, const char *tool, const char *args) {
  size_t i = find(s, tool);
  if (i == ASTOOLS_SELECTION_MAX) return ASTOOLS_ERR_DENIED;
  const astools_selected_command *v = &s->commands[i];
  return astools_validate_impl(s->ctx, v->ref, v->command, args, s->views[i].tool->content_sha256);
}
