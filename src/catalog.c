/*
 * catalog.c — budgeted catalog rendering (§7.1).
 *
 * Determinism: identical registry state and configuration produce
 * byte-identical text. Tool order is cfg.priority ids first (in priority
 * order), then the remaining ids ascending by strcmp; commands render in
 * manifest order. Disabled/unavailable tools never render. When several
 * versions of one id are listed, the highest non-pre-release wins (the
 * highest pre-release when nothing else exists), mirroring bare-ref
 * resolution (§3.7).
 *
 * Budget algorithm (char_budget 0 = unlimited; "characters" are UTF-8
 * bytes): render every selected tool at the requested level; when the
 * text exceeds the budget, degrade ALL tools one level and re-render
 * (full -> summary -> index); once at index and still over budget, drop
 * whole tools from the TAIL of the ordering one at a time and re-render
 * until the text fits. Output is never truncated mid-tool; the fixed
 * header renders even when it alone exceeds the budget, because nothing
 * smaller exists.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expr)                                                            \
  do {                                                                       \
    astools_err try_e_ = (expr);                                             \
    if (try_e_ != ASTOOLS_OK) return try_e_;                                 \
  } while (0)

static const char k_header[] =
    "## Tools\n"
    "\n"
    "You can act on the machine with the tools below. To use one, write\n"
    "a single line:  CALL <tool>.<command> {<args>}\n";

/* ---- tool selection (caller holds c->lock for reading) ------------------ */

static bool tool_listed(const astools_tool *t) {
  return t != NULL && t->available && t->enabled && t->m != NULL &&
         t->m->id != NULL;
}

/* Release beats pre-release; then higher SemVer. Ties keep the incumbent
 * (first root wins, §4.1). */
static bool version_better(const astools_tool *cand, const astools_tool *cur) {
  bool cand_rel = cand->ver.prerelease == NULL;
  bool cur_rel = cur->ver.prerelease == NULL;
  if (cand_rel != cur_rel) return cand_rel;
  return astools_semver_cmp(&cand->ver, &cur->ver) > 0;
}

static int cmp_tool_id(const void *a, const void *b) {
  const astools_tool *ta = *(const astools_tool *const *)a;
  const astools_tool *tb = *(const astools_tool *const *)b;
  return strcmp(ta->m->id, tb->m->id);
}

static astools_err collect_tools(astools_ctx *c, astools_tool ***out_list,
                                 size_t *out_n) {
  astools_tool **sel = NULL, **ordered = NULL;
  bool *taken = NULL;
  size_t sel_n = 0, i, j, k, tail;

  *out_list = NULL;
  *out_n = 0;
  if (c->tools_n == 0) return ASTOOLS_OK;
  sel = malloc(c->tools_n * sizeof *sel);
  if (!sel) return ASTOOLS_ERR_NOMEM;
  for (i = 0; i < c->tools_n; i++) {
    astools_tool *t = c->tools[i];
    if (!tool_listed(t)) continue;
    for (j = 0; j < sel_n; j++)
      if (strcmp(sel[j]->m->id, t->m->id) == 0) break;
    if (j < sel_n) {
      if (version_better(t, sel[j])) sel[j] = t;
    } else {
      sel[sel_n++] = t;
    }
  }
  if (sel_n == 0) {
    free(sel);
    return ASTOOLS_OK;
  }
  ordered = malloc(sel_n * sizeof *ordered);
  taken = calloc(sel_n, sizeof *taken);
  if (!ordered || !taken) {
    free(sel);
    free(ordered);
    free(taken);
    return ASTOOLS_ERR_NOMEM;
  }
  k = 0;
  for (i = 0; i < c->cfg.priority_len; i++) {
    const char *pid = c->cfg.priority[i];
    if (!pid) continue;
    for (j = 0; j < sel_n; j++) {
      if (!taken[j] && strcmp(sel[j]->m->id, pid) == 0) {
        ordered[k++] = sel[j];
        taken[j] = true;
        break;
      }
    }
  }
  tail = k;
  for (j = 0; j < sel_n; j++)
    if (!taken[j]) ordered[k++] = sel[j];
  qsort(ordered + tail, k - tail, sizeof *ordered, cmp_tool_id);
  free(sel);
  free(taken);
  *out_list = ordered;
  *out_n = k;
  return ASTOOLS_OK;
}

/* ---- text helpers ------------------------------------------------------- */

/* Append s forced onto one line: '\n' becomes ' ', '\r' is dropped.
 * Manifests are untrusted; single-line slots must stay single-line. */
static astools_err append_oneline(astools_buf *b, const char *s) {
  if (!s) return ASTOOLS_OK;
  for (; *s; s++) {
    char ch = *s;
    if (ch == '\r') continue;
    if (ch == '\n') ch = ' ';
    TRY(astools_buf_appendc(b, ch));
  }
  return ASTOOLS_OK;
}

static void buf_rtrim(astools_buf *b) {
  while (b->len > 0 &&
         (b->data[b->len - 1] == ' ' || b->data[b->len - 1] == '\t'))
    b->len--;
  if (b->data) b->data[b->len] = '\0';
}

/* Append prose line by line: surrounding blank space trimmed, non-empty
 * lines prefixed with indent, interior blank lines kept bare. */
static astools_err append_prose(astools_buf *b, const char *s,
                                const char *indent) {
  char *copy, *line, *next;
  astools_err e = ASTOOLS_OK;

  copy = astools_strdup(s);
  if (!copy) return ASTOOLS_ERR_NOMEM;
  astools_str_trim(copy);
  if (copy[0] == '\0') {
    free(copy);
    return ASTOOLS_OK;
  }
  line = copy;
  while (line) {
    size_t ll;
    next = strchr(line, '\n');
    if (next) *next++ = '\0';
    ll = strlen(line);
    while (ll > 0 && (line[ll - 1] == '\r' || line[ll - 1] == ' ' ||
                      line[ll - 1] == '\t'))
      line[--ll] = '\0';
    if (ll > 0) {
      e = astools_buf_appends(b, indent);
      if (e == ASTOOLS_OK) e = astools_buf_appends(b, line);
    }
    if (e == ASTOOLS_OK) e = astools_buf_appendc(b, '\n');
    if (e != ASTOOLS_OK) break;
    line = next;
  }
  free(copy);
  return e;
}

/* string/integer/number/boolean/bytes/datetime/duration/uuid/path/
 * enum(v1|v2)/array<item>/map<value>/object. Depth-capped defensively. */
static astools_err append_type_name(astools_buf *b, const astools_type *t,
                                    int depth) {
  size_t i;
  if (!t || depth > 32) return astools_buf_appends(b, "object");
  switch (t->kind) {
  case AT_STRING:
    return astools_buf_appends(b, "string");
  case AT_INTEGER:
    return astools_buf_appends(b, "integer");
  case AT_NUMBER:
    return astools_buf_appends(b, "number");
  case AT_BOOLEAN:
    return astools_buf_appends(b, "boolean");
  case AT_BYTES:
    return astools_buf_appends(b, "bytes");
  case AT_DATETIME:
    return astools_buf_appends(b, "datetime");
  case AT_DURATION:
    return astools_buf_appends(b, "duration");
  case AT_UUID:
    return astools_buf_appends(b, "uuid");
  case AT_PATH:
    return astools_buf_appends(b, "path");
  case AT_ENUM:
    TRY(astools_buf_appends(b, "enum("));
    for (i = 0; i < t->values_len; i++) {
      if (i > 0) TRY(astools_buf_appendc(b, '|'));
      TRY(append_oneline(b, t->values[i]));
    }
    return astools_buf_appendc(b, ')');
  case AT_ARRAY:
    TRY(astools_buf_appends(b, "array<"));
    TRY(append_type_name(b, t->item, depth + 1));
    return astools_buf_appendc(b, '>');
  case AT_MAP:
    TRY(astools_buf_appends(b, "map<"));
    TRY(append_type_name(b, t->value, depth + 1));
    return astools_buf_appendc(b, '>');
  case AT_OBJECT:
  default:
    return astools_buf_appends(b, "object");
  }
}

/* Fixed marker order: read-only, destructive, idempotent, long-running,
 * deprecated. Renders "  [a] [b]" or nothing. */
static astools_err append_markers(astools_buf *b, const astools_cmd *cmd) {
  const char *names[5];
  bool flags[5];
  size_t i;
  bool first = true;

  names[0] = "read-only";
  flags[0] = cmd->read_only;
  names[1] = "destructive";
  flags[1] = cmd->destructive;
  names[2] = "idempotent";
  flags[2] = cmd->idempotent;
  names[3] = "long-running";
  flags[3] = cmd->long_running;
  names[4] = "deprecated";
  flags[4] = cmd->deprecated;
  for (i = 0; i < 5; i++) {
    if (!flags[i]) continue;
    TRY(astools_buf_appends(b, first ? "  [" : " ["));
    TRY(astools_buf_appends(b, names[i]));
    TRY(astools_buf_appendc(b, ']'));
    first = false;
  }
  return ASTOOLS_OK;
}

/* ---- rendering ---------------------------------------------------------- */

static astools_err render_cmd(astools_buf *b, const astools_manifest *m,
                              const astools_cmd *cmd, int lvl) {
  size_t i;

  /* index line: "- <tool>.<name> {p1, p2?}  [markers]" */
  TRY(astools_buf_printf(b, "- %s.%s {", m->id, cmd->name));
  for (i = 0; i < cmd->params_len; i++) {
    const astools_param *p = &cmd->params[i];
    if (i > 0) TRY(astools_buf_appends(b, ", "));
    TRY(append_oneline(b, p->name));
    if (!p->required) TRY(astools_buf_appendc(b, '?'));
  }
  TRY(astools_buf_appendc(b, '}'));
  TRY(append_markers(b, cmd));
  TRY(astools_buf_appendc(b, '\n'));
  if (lvl < ASTOOLS_CATALOG_SUMMARY) return ASTOOLS_OK;

  if (!astools_str_blank(cmd->summary)) {
    TRY(astools_buf_appends(b, "    "));
    TRY(append_oneline(b, cmd->summary));
    TRY(astools_buf_appendc(b, '\n'));
  }
  for (i = 0; i < cmd->params_len; i++) {
    const astools_param *p = &cmd->params[i];
    TRY(astools_buf_appends(b, "    params: "));
    TRY(append_oneline(b, p->name));
    TRY(astools_buf_appends(b, " ("));
    TRY(append_type_name(b, p->type, 0));
    if (p->required) TRY(astools_buf_appends(b, ", required"));
    TRY(astools_buf_appendc(b, ')'));
    if (!astools_str_blank(p->description)) {
      TRY(astools_buf_appends(b, " \xe2\x80\x94 "));
      TRY(append_oneline(b, p->description));
    }
    TRY(astools_buf_appendc(b, '\n'));
  }
  if (!astools_str_blank(cmd->returns_desc)) {
    TRY(astools_buf_appends(b, "    Returns "));
    TRY(append_oneline(b, cmd->returns_desc));
    TRY(astools_buf_appendc(b, '\n'));
  }
  if (lvl < ASTOOLS_CATALOG_FULL) return ASTOOLS_OK;

  if (!astools_str_blank(cmd->description))
    TRY(append_prose(b, cmd->description, "    "));
  /* first example with a call object, rendered as a worked call line */
  for (i = 0; i < cmd->examples_len; i++) {
    char *args;
    astools_err e;
    if (!cmd->examples[i].call) continue;
    args = astools_xcdn_write_node(cmd->examples[i].call, false);
    if (!args) return ASTOOLS_ERR_NOMEM;
    e = astools_buf_printf(b, "    e.g.  CALL %s.%s ", m->id, cmd->name);
    if (e == ASTOOLS_OK) e = append_oneline(b, args);
    free(args);
    if (e != ASTOOLS_OK) return e;
    buf_rtrim(b);
    TRY(astools_buf_appendc(b, '\n'));
    break;
  }
  return ASTOOLS_OK;
}

static astools_err render_tool(astools_buf *b, const astools_tool *t,
                               int lvl) {
  const astools_manifest *m = t->m;
  size_t i;

  TRY(astools_buf_printf(b, "\n### %s %s", m->id,
                         m->version ? m->version : "0.0.0"));
  if (!astools_str_blank(m->title)) {
    TRY(astools_buf_appends(b, " \xe2\x80\x94 "));
    TRY(append_oneline(b, m->title));
  }
  TRY(astools_buf_appendc(b, '\n'));
  if (lvl >= ASTOOLS_CATALOG_SUMMARY && !astools_str_blank(m->summary)) {
    TRY(append_oneline(b, m->summary));
    TRY(astools_buf_appendc(b, '\n'));
  }
  if (lvl >= ASTOOLS_CATALOG_FULL && !astools_str_blank(m->description))
    TRY(append_prose(b, m->description, ""));
  for (i = 0; i < m->commands_len; i++)
    TRY(render_cmd(b, m, &m->commands[i], lvl));
  return ASTOOLS_OK;
}

static astools_err render_catalog(astools_tool **list, size_t keep, int lvl,
                                  astools_buf *b) {
  size_t i;
  TRY(astools_buf_appends(b, k_header));
  for (i = 0; i < keep; i++) TRY(render_tool(b, list[i], lvl));
  return ASTOOLS_OK;
}

/* ---- entry point -------------------------------------------------------- */

astools_err astools_catalog_render(astools_ctx *c, astools_catalog_level lvl,
                                   size_t char_budget, char **out) {
  astools_tool **list = NULL;
  size_t n = 0, keep;
  int eff;
  astools_buf b;
  astools_err e;

  if (!c || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  eff = (int)lvl;
  if (eff < ASTOOLS_CATALOG_INDEX) eff = ASTOOLS_CATALOG_INDEX;
  if (eff > ASTOOLS_CATALOG_FULL) eff = ASTOOLS_CATALOG_FULL;

  astools_buf_init(&b);
  os_rwlock_rdlock(&c->lock);
  e = collect_tools(c, &list, &n);
  if (e != ASTOOLS_OK) goto done;
  keep = n;
  for (;;) {
    astools_buf_free(&b);
    astools_buf_init(&b);
    e = render_catalog(list, keep, eff, &b);
    if (e != ASTOOLS_OK) goto done;
    if (char_budget == 0 || b.len <= char_budget) break;
    if (eff > ASTOOLS_CATALOG_INDEX) {
      eff--; /* degrade every tool one level and retry */
      continue;
    }
    if (keep > 0) {
      keep--; /* drop whole tools from the tail */
      continue;
    }
    break; /* the bare header exceeds the budget; emit it whole */
  }

done:
  os_rwlock_rdunlock(&c->lock);
  free(list);
  if (e != ASTOOLS_OK) {
    astools_buf_free(&b);
    return astools_seterr(c, e, "catalog: render failed");
  }
  *out = astools_buf_detach(&b);
  if (!*out)
    return astools_seterr(c, ASTOOLS_ERR_NOMEM, "catalog: out of memory");
  return ASTOOLS_OK;
}
