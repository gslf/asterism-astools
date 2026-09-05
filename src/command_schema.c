/* Export application-independent command contracts from the typed manifest. */
#include "astools_internal.h"
#include <stdlib.h>

astools_err astools_command_schemas(astools_ctx *c, char **out) {
  astools_tool **tools = NULL;
  size_t n = 0, count = 0;
  astools_buf b;
  astools_err e;
  if (!c || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  astools_buf_init(&b);
  os_rwlock_rdlock(&c->lock);
  e = astools_collect_tools(c, &tools, &n);
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, "[");
  for (size_t i = 0; e == ASTOOLS_OK && i < n; i++) {
    astools_manifest *m = tools[i]->m;
    for (size_t j = 0; e == ASTOOLS_OK && j < m->commands_len; j++) {
      char *schema = NULL;
      const astools_cmd *cmd = &m->commands[j];
      e = astools_jschema_input(cmd, &schema);
      if (e == ASTOOLS_OK)
        e = astools_buf_printf(&b, "%s{\"tool\":\"%s.%s\",\"arguments\":%s}",
                               count++ ? "," : "", m->id, cmd->name, schema);
      free(schema);
    }
  }
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, "]");
  free(tools);
  os_rwlock_rdunlock(&c->lock);
  if (e == ASTOOLS_OK) *out = astools_buf_detach(&b);
  astools_buf_free(&b);
  return e;
}
