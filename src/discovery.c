/* Bounded lexical discovery. Scores rank metadata, never permission grants. */
#include "discovery.h"
#include <stdlib.h>
#include <string.h>
typedef struct {
  astools_command_view view;
  char *schema;
  size_t cost;
  int score;
} candidate;
static int folded(unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
static int contains(const char *text, const char *term, size_t n) {
  if (!text) return 0;
  size_t len = strlen(text);
  if (len > 4096) len = 4096;
  for (size_t i = 0; i + n <= len; i++) {
    size_t j = 0;
    while (j < n && folded((unsigned char)text[i + j]) == folded((unsigned char)term[j]))
      j++;
    if (j == n) return 1;
  }
  return 0;
}
static int word(unsigned char c) {
  return c >= 128 || (c >= '0' && c <= '9') || (folded(c) >= 'a' && folded(c) <= 'z') || c == '_' ||
         c == '-';
}
static int score(const char *query, const astools_manifest *m, const astools_cmd *cmd) {
  int result = 0, terms = 0;
  for (const char *p = query ? query : ""; *p && terms < 32;) {
    while (*p && !word((unsigned char)*p))
      p++;
    const char *start = p;
    while (word((unsigned char)*p))
      p++;
    size_t n = (size_t)(p - start);
    if (n < 2) continue;
    result += 12 * contains(m->id, start, n) + 12 * contains(cmd->name, start, n) +
              3 * contains(cmd->summary, start, n) + contains(m->summary, start, n);
    terms++;
  }
  return result;
}
static int allowed(const astools_discovery_options *o, const char *id, const char *cmd) {
  if (!o->allow) return 1;
  for (size_t i = 0; i < o->allow_count; i++) {
    const char *s = o->allow[i];
    size_t n = strlen(id);
    if (s && !strncmp(s, id, n) && s[n] == '.' && !strcmp(s + n + 1, cmd)) return 1;
  }
  return 0;
}
/* Exclude commands for which no legal required path argument can exist.
 * Concrete containment/default/existence checks remain in invocation policy. */
static int possible(const astools_type *type, const astools_effective *eff, unsigned depth) {
  if (!type || depth > 32) return 0;
  if (type->kind == AT_PATH) {
    for (size_t i = 0; i < eff->fs_len; i++)
      if (eff->fs[i].path && (type->access & ~eff->fs[i].access) == 0) return 1;
    return 0;
  }
  if (type->kind == AT_ARRAY && type->min_items > 0) return possible(type->item, eff, depth + 1);
  if (type->kind == AT_OBJECT)
    for (size_t i = 0; i < type->fields_len; i++)
      if ((type->fields[i].required || type->fields[i].dflt) &&
          !possible(type->fields[i].type, eff, depth + 1))
        return 0;
  return 1;
}
static int command_possible(const astools_cmd *cmd, const astools_effective *eff) {
  for (size_t i = 0; i < cmd->params_len; i++)
    if ((cmd->params[i].required || cmd->params[i].dflt) && !possible(cmd->params[i].type, eff, 0))
      return 0;
  return 1;
}
static char *qualified(const char *a, char separator, const char *b) {
  size_t n = strlen(a) + strlen(b) + 2;
  char *text = malloc(n);
  if (text) snprintf(text, n, "%s%c%s", a, separator, b);
  return text;
}
static astools_err materialize(astools_selection *s, candidate *v, size_t n, size_t limit,
                               size_t budget) {
  astools_buf json;
  astools_buf_init(&json);
  astools_err e = astools_buf_appends(&json, "[");
  for (size_t i = 0; i < n && s->count < limit && e == ASTOOLS_OK; i++) {
    if (v[i].cost > budget) continue;
    budget -= v[i].cost;
    size_t k = s->count++;
    astools_command_view *view = &s->views[k];
    *view = v[i].view;
    astools_tool_ref(view->tool);
    astools_manifest *m = view->tool->m;
    astools_selected_command *out = &s->commands[k];
    out->tool = qualified(m->id, '.', view->command->name);
    out->ref = qualified(m->id, '@', m->version);
    out->command = view->command->name;
    out->read_only = view->command->read_only;
    out->destructive = view->command->destructive;
    out->idempotent = view->command->idempotent;
    out->long_running = view->command->long_running;
    const char *summary = view->command->summary ? view->command->summary : "";
    size_t length = strlen(summary);
    if (length > 1024) {
      length = 1024;
      while (length && ((unsigned char)summary[length] & 0xc0) == 0x80)
        length--;
    }
    out->summary = astools_strndup(summary, length);
    out->arguments = v[i].schema;
    v[i].schema = NULL;
    char *hash = malloc(65);
    out->content_sha256 = hash;
    if (!out->tool || !out->ref || !out->summary || !hash) {
      e = ASTOOLS_ERR_NOMEM;
      break;
    }
    for (size_t b = 0; b < 32; b++)
      snprintf(hash + b * 2, 3, "%02x", view->tool->content_sha256[b]);
    e = astools_buf_printf(&json, "%s{\"tool\":\"%s\",\"arguments\":%s}", k ? "," : "", out->tool,
                           out->arguments);
  }
  if (e == ASTOOLS_OK) e = astools_buf_appends(&json, "]");
  if (e == ASTOOLS_OK) s->schemas = astools_buf_detach(&json);
  if (e == ASTOOLS_OK)
    e = astools_catalog_commands(s->views, s->count, ASTOOLS_CATALOG_INDEX, &s->catalog);
  if (e == ASTOOLS_OK) e = astools_gbnf_commands(s->views, s->count, &s->grammar);
  astools_buf_free(&json);
  return e;
}
astools_err astools_discover(astools_ctx *c, const astools_discovery_options *options,
                             astools_selection **out) {
  astools_discovery_options o = options ? *options : (astools_discovery_options){0};
  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  if (!c || o.limit > 64 || o.schema_budget > 1024 * 1024 || o.allow_count > 4096 ||
      (o.allow_count && !o.allow) || (o.intent && strlen(o.intent) > 4096))
    return ASTOOLS_ERR_INVALID;
  if (!o.limit) o.limit = 16;
  if (!o.schema_budget) o.schema_budget = 24000;
  astools_selection *s = calloc(1, sizeof *s);
  if (!s) return ASTOOLS_ERR_NOMEM;
  s->ctx = c;
  astools_tool **tools = NULL;
  size_t tools_n = 0, n = 0;
  candidate best[64] = {0};
  os_rwlock_rdlock(&c->lock);
  astools_err e = astools_collect_tools(c, &tools, &tools_n);
  s->revision = c->registry_revision;
  for (size_t i = 0; i < tools_n; i++)
    astools_tool_ref(tools[i]);
  os_rwlock_rdunlock(&c->lock);
  for (size_t i = 0; i < tools_n && e == ASTOOLS_OK; i++) {
    astools_tool *t = tools[i];
    astools_effective eff = {0};
    if (t->m->kind == ASTOOLS_KIND_LIBRARY &&
        (!c->cfg.allow_library || t->trust != ASTOOLS_TRUST_FULL))
      continue;
    if (t->m->kind == ASTOOLS_KIND_EXECUTABLE && t->m->mode == ASTOOLS_MODE_PERSISTENT &&
        c->no_threads)
      continue;
    e = astools_policy_effective(c, t, &eff);
    int denied = (t->m->perms.proc && !eff.proc) || (t->m->perms.net && !eff.net);
    if (e != ASTOOLS_OK || denied) {
      astools_effective_free(&eff);
      continue;
    }
    for (size_t j = 0; j < t->m->commands_len; j++) {
      const astools_cmd *cmd = &t->m->commands[j];
      if (o.read_only && (!cmd->read_only || cmd->destructive)) continue;
      if (!allowed(&o, t->m->id, cmd->name) || !command_possible(cmd, &eff)) continue;
      s->candidates++;
      int relevance = score(o.intent, t->m, cmd);
      if (n == 64 && relevance <= best[n - 1].score) continue;
      char *schema = NULL;
      e = astools_jschema_input(cmd, &schema);
      if (e != ASTOOLS_OK) break;
      size_t cost = strlen(schema) + strlen(t->m->id) + strlen(cmd->name) + 128;
      if (cmd->summary) cost += strlen(cmd->summary) < 1024 ? strlen(cmd->summary) : 1024;
      if (cost > o.schema_budget) {
        free(schema);
        continue;
      }
      size_t k = n;
      if (k == 64) {
        free(best[--k].schema);
      } else
        n++;
      while (k && relevance > best[k - 1].score) {
        best[k] = best[k - 1];
        k--;
      }
      best[k] = (candidate){{t, cmd}, schema, cost, relevance};
    }
    astools_effective_free(&eff);
  }
  if (e == ASTOOLS_OK) e = materialize(s, best, n, o.limit, o.schema_budget);
  for (size_t i = 0; i < n; i++)
    free(best[i].schema);
  for (size_t i = 0; i < tools_n; i++)
    astools_tool_unref(tools[i]);
  free(tools);
  if (e != ASTOOLS_OK) {
    astools_selection_free(s);
    return e;
  }
  s->omitted = s->candidates - s->count;
  *out = s;
  return ASTOOLS_OK;
}
