/*
 * gbnf.c — GBNF grammar export (§7.3, Appendix C).
 *
 * The exported grammar's language is exactly the valid call lines of the
 * enabled registry: one production per (tool,command) in catalog order
 * (cfg.priority ids first, remaining ids ascending, commands in manifest
 * order), so identical registries yield byte-identical grammars.
 *
 * Rule naming: "t-<tool>-c-<cmd>" (GBNF names cannot contain '.' or '_';
 * ids already use '-'). Because '-' is also a legal slug character the
 * prefix scheme alone is not injective for pathological ids (tool "a-c-b"
 * cmd "x" vs tool "a" cmd "b-c-x"); collisions are resolved
 * deterministically by appending "-2", "-3", ... in emission order.
 *
 * Required params are emitted first, in manifest order, as fixed
 * '"name: " value' sequences joined by ", " literals; every optional
 * param follows as its own ( ", name: " value )? group. Simplification
 * (documented per §7.3 note): when a command has NO required params the
 * leading-comma placement of the first present optional cannot be
 * expressed by plain concatenation, so the whole args object degrades to
 * the generic balanced single-line "obj" production — the grammar still
 * guarantees a syntactically plausible call line and the engine validates
 * semantics anyway. A command with no params at all emits the exact
 * literal "{}".
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

#define TRY(expr)                                                            \
  do {                                                                       \
    astools_err try_e_ = (expr);                                             \
    if (try_e_ != ASTOOLS_OK) return try_e_;                                 \
  } while (0)

/* Shared terminal rules, emitted once at the bottom. "obj" is a balanced-
 * braces single-line object approximation (strings may contain braces). */
static const char k_terminals[] =
    "str ::= \"\\\"\" char* \"\\\"\"\n"
    "char ::= [^\"\\\\\\n\\r] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F]"
    " [0-9a-fA-F] [0-9a-fA-F] [0-9a-fA-F])\n"
    "int ::= \"-\"? [0-9]+\n"
    "num ::= \"-\"? [0-9]+ (\".\" [0-9]+)? ((\"e\" | \"E\") (\"+\" | \"-\")?"
    " [0-9]+)?\n"
    "ws ::= [ ]*\n"
    "obj ::= \"{\" ([^{}\"\\n\\r] | str | obj)* \"}\"\n";

/* ---- tool selection (caller holds c->lock for reading) ------------------ */
/* Duplicated from catalog.c: the frozen internal header declares no shared
 * collection helper, and both renderings must use the same ordering. */

static bool tool_listed(const astools_tool *t) {
  return t != NULL && t->available && t->enabled && t->m != NULL &&
         t->m->id != NULL;
}

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

/* ---- rule table --------------------------------------------------------- */

typedef struct {
  const astools_manifest *m;
  const astools_cmd *cmd;
  char *name; /* malloc'd unique rule name */
} gbnf_rule;

static void free_rules(gbnf_rule *rules, size_t n) {
  size_t i;
  if (!rules) return;
  for (i = 0; i < n; i++) free(rules[i].name);
  free(rules);
}

static char *make_rule_name(const char *tool, const char *cmd,
                            const gbnf_rule *prev, size_t prev_n) {
  unsigned long suffix;

  for (suffix = 1; suffix <= 1000000UL; suffix++) {
    astools_buf b;
    char *cand;
    size_t i;
    bool clash = false;
    astools_err e;

    astools_buf_init(&b);
    if (suffix == 1)
      e = astools_buf_printf(&b, "t-%s-c-%s", tool, cmd);
    else
      e = astools_buf_printf(&b, "t-%s-c-%s-%lu", tool, cmd, suffix);
    if (e != ASTOOLS_OK) {
      astools_buf_free(&b);
      return NULL;
    }
    cand = astools_buf_detach(&b);
    if (!cand) return NULL;
    for (i = 0; i < prev_n; i++)
      if (strcmp(prev[i].name, cand) == 0) {
        clash = true;
        break;
      }
    if (!clash) return cand;
    free(cand);
  }
  return NULL;
}

static astools_err build_rules(astools_tool **list, size_t n, gbnf_rule **out,
                               size_t *out_n) {
  size_t total = 0, i, j, k = 0;
  gbnf_rule *rules;

  *out = NULL;
  *out_n = 0;
  for (i = 0; i < n; i++) total += list[i]->m->commands_len;
  if (total == 0) return ASTOOLS_OK;
  rules = calloc(total, sizeof *rules);
  if (!rules) return ASTOOLS_ERR_NOMEM;
  for (i = 0; i < n; i++) {
    const astools_manifest *m = list[i]->m;
    for (j = 0; j < m->commands_len; j++) {
      char *name = make_rule_name(m->id, m->commands[j].name, rules, k);
      if (!name) {
        free_rules(rules, k);
        return ASTOOLS_ERR_NOMEM;
      }
      rules[k].m = m;
      rules[k].cmd = &m->commands[j];
      rules[k].name = name;
      k++;
    }
  }
  *out = rules;
  *out_n = k;
  return ASTOOLS_OK;
}

/* ---- value productions -------------------------------------------------- */

/* GBNF literal for the exact xCDN string token "<v>": the value is
 * xCDN-escaped first, then that token is GBNF-escaped. Example: value
 * utf8 emits "\"utf8\"" (the grammar matches the five bytes "utf8"
 * including quotes). */
static astools_err emit_enum_lit(astools_buf *b, const char *v) {
  TRY(astools_buf_appendc(b, '"'));
  TRY(astools_buf_appends(b, "\\\"")); /* xCDN opening quote */
  for (; *v; v++) {
    switch (*v) {
    case '"':
      TRY(astools_buf_appends(b, "\\\\\\\"")); /* token holds \" */
      break;
    case '\\':
      TRY(astools_buf_appends(b, "\\\\\\\\")); /* token holds \\ */
      break;
    case '\n':
      TRY(astools_buf_appends(b, "\\\\n"));
      break;
    case '\r':
      TRY(astools_buf_appends(b, "\\\\r"));
      break;
    case '\t':
      TRY(astools_buf_appends(b, "\\\\t"));
      break;
    default:
      TRY(astools_buf_appendc(b, *v));
      break;
    }
  }
  TRY(astools_buf_appends(b, "\\\"")); /* xCDN closing quote */
  return astools_buf_appendc(b, '"');
}

/* Production matching one argument value of type t. Depth-capped
 * defensively against hostile deeply-nested manifests. */
static astools_err emit_value(astools_buf *b, const astools_type *t,
                              int depth) {
  size_t i;
  if (!t || depth > 32) return astools_buf_appends(b, "obj");
  switch (t->kind) {
  case AT_STRING:
  case AT_PATH:
    return astools_buf_appends(b, "str");
  case AT_INTEGER:
    return astools_buf_appends(b, "int");
  case AT_NUMBER:
    return astools_buf_appends(b, "num");
  case AT_BOOLEAN:
    return astools_buf_appends(b, "(\"true\" | \"false\")");
  case AT_BYTES:
    return astools_buf_appends(b, "\"b\" str");
  case AT_DATETIME:
    return astools_buf_appends(b, "\"t\" str");
  case AT_DURATION:
    return astools_buf_appends(b, "\"r\" str");
  case AT_UUID:
    return astools_buf_appends(b, "\"u\" str");
  case AT_ENUM:
    if (t->values_len == 0) return astools_buf_appends(b, "str");
    TRY(astools_buf_appendc(b, '('));
    for (i = 0; i < t->values_len; i++) {
      if (i > 0) TRY(astools_buf_appends(b, " | "));
      TRY(emit_enum_lit(b, t->values[i] ? t->values[i] : ""));
    }
    return astools_buf_appendc(b, ')');
  case AT_ARRAY:
    TRY(astools_buf_appends(b, "\"[\" ( "));
    TRY(emit_value(b, t->item, depth + 1));
    TRY(astools_buf_appends(b, " ( \",\" ws "));
    TRY(emit_value(b, t->item, depth + 1));
    TRY(astools_buf_appends(b, " )* )? \"]\""));
    return ASTOOLS_OK;
  case AT_MAP:
  case AT_OBJECT:
  default:
    return astools_buf_appends(b, "obj");
  }
}

static astools_err emit_cmd_rule(astools_buf *b, const gbnf_rule *ge) {
  const astools_cmd *cmd = ge->cmd;
  const char *id = ge->m->id, *cn = cmd->name;
  size_t i, nreq = 0;
  bool first = true;

  for (i = 0; i < cmd->params_len; i++)
    if (cmd->params[i].required) nreq++;
  TRY(astools_buf_printf(b, "%s ::= ", ge->name));
  if (cmd->params_len == 0)
    return astools_buf_printf(b, "\"%s.%s {}\"\n", id, cn);
  if (nreq == 0)
    /* zero required params: generic object (see header comment) */
    return astools_buf_printf(b, "\"%s.%s \" obj\n", id, cn);
  TRY(astools_buf_printf(b, "\"%s.%s {\"", id, cn));
  for (i = 0; i < cmd->params_len; i++) {
    const astools_param *p = &cmd->params[i];
    if (!p->required) continue;
    if (first) {
      TRY(astools_buf_printf(b, " \"%s: \" ", p->name));
      first = false;
    } else {
      TRY(astools_buf_printf(b, " \", %s: \" ", p->name));
    }
    TRY(emit_value(b, p->type, 0));
  }
  for (i = 0; i < cmd->params_len; i++) {
    const astools_param *p = &cmd->params[i];
    if (p->required) continue;
    TRY(astools_buf_printf(b, " ( \", %s: \" ", p->name));
    TRY(emit_value(b, p->type, 0));
    TRY(astools_buf_appends(b, " )?"));
  }
  TRY(astools_buf_appends(b, " \"}\"\n"));
  return ASTOOLS_OK;
}

/* ---- entry point -------------------------------------------------------- */

astools_err astools_gbnf_render(astools_ctx *c, char **out) {
  astools_tool **list = NULL;
  gbnf_rule *rules = NULL;
  size_t n = 0, nrules = 0, i;
  astools_buf b;
  astools_err e;

  if (!c || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  astools_buf_init(&b);
  os_rwlock_rdlock(&c->lock);
  e = collect_tools(c, &list, &n);
  if (e == ASTOOLS_OK) e = build_rules(list, n, &rules, &nrules);
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&b, "root ::= \"CALL \" call \"\\n\"\n");
  if (e == ASTOOLS_OK) {
    if (nrules == 0) {
      /* empty registry: no valid calls exist; keep the grammar well-formed
       * with a language of just "CALL \n" */
      e = astools_buf_appends(&b, "call ::= \"\"\n");
    } else {
      e = astools_buf_appends(&b, "call ::= ");
      for (i = 0; i < nrules && e == ASTOOLS_OK; i++) {
        if (i > 0) e = astools_buf_appends(&b, " | ");
        if (e == ASTOOLS_OK) e = astools_buf_appends(&b, rules[i].name);
      }
      if (e == ASTOOLS_OK) e = astools_buf_appendc(&b, '\n');
    }
  }
  for (i = 0; i < nrules && e == ASTOOLS_OK; i++)
    e = emit_cmd_rule(&b, &rules[i]);
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, k_terminals);
  os_rwlock_rdunlock(&c->lock);
  free(list);
  free_rules(rules, nrules);
  if (e != ASTOOLS_OK) {
    astools_buf_free(&b);
    return astools_seterr(c, e, "gbnf: render failed");
  }
  *out = astools_buf_detach(&b);
  if (!*out)
    return astools_seterr(c, ASTOOLS_ERR_NOMEM, "gbnf: out of memory");
  return ASTOOLS_OK;
}
