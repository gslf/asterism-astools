/* The adapter accepts a fixed read-only API, independent of server metadata. */
#include "astools_internal.h"
#include <string.h>

bool astools_lsp_manifest_valid(const astools_manifest *m) {
  if (m->protocol != ASTOOLS_PROTOCOL_LSP) return true;
  for (size_t i = 0; i < m->commands_len; i++) {
    const astools_cmd *cmd = &m->commands[i];
    bool position = !strcmp(cmd->name, "definition") || !strcmp(cmd->name, "references");
    if ((!position && strcmp(cmd->name, "symbols") && strcmp(cmd->name, "diagnostics")) ||
        !cmd->read_only || cmd->destructive || cmd->idempotent ||
        cmd->params_len != (position ? 4u : 2u))
      return false;
    unsigned fields = 0;
    for (size_t j = 0; j < cmd->params_len; j++) {
      const astools_param *p = &cmd->params[j];
      const astools_type *t = p->type;
      if (!p->required || p->dflt || !t) return false;
      if (!strcmp(p->name, "path") && t->kind == AT_PATH && t->access == ASTOOLS_ACCESS_READ &&
          t->must_exist)
        fields |= 1;
      else if (!strcmp(p->name, "sha256") && t->kind == AT_STRING && t->min_len == 64 &&
               t->max_len == 64)
        fields |= 2;
      else if (position && t->kind == AT_INTEGER && t->has_min && t->min_num == 1 && t->has_max &&
               t->max_num == INT32_MAX) {
        if (!strcmp(p->name, "line")) fields |= 4;
        else if (!strcmp(p->name, "column")) fields |= 8;
        else return false;
      } else return false;
    }
    if (fields != (position ? 15u : 3u)) return false;
  }
  return true;
}
