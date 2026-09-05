/*
 * gbnf.c — GBNF grammar export.
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
 * (documented per note): when a command has NO required params the
 * leading-comma placement of the first present optional cannot be
 * expressed by plain concatenation, so the whole args object degrades to
 * the generic balanced single-line "obj" production — the grammar still
 * guarantees a syntactically plausible call line and the engine validates
 * semantics anyway. A command with no params at all emits the exact
 * literal "{}".
 */

#include "discovery.h"

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

static astools_err build_rules(const astools_command_view *commands, size_t count,
    gbnf_rule **out, size_t *out_n) {
  *out = NULL; *out_n = 0;
  if (!count) return ASTOOLS_OK;
  gbnf_rule *rules = calloc(count,sizeof *rules);
  if (!rules) return ASTOOLS_ERR_NOMEM;
  for (size_t i = 0; i < count; i++) {
    const astools_manifest *m = commands[i].tool->m;
    const astools_cmd *cmd = commands[i].command;
    char *name = make_rule_name(m->id,cmd->name,rules,i);
    if (!name) { free_rules(rules,i); return ASTOOLS_ERR_NOMEM; }
    rules[i].m = m; rules[i].cmd = cmd; rules[i].name = name;
  }
  *out = rules; *out_n = count; return ASTOOLS_OK;
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

astools_err astools_gbnf_commands(const astools_command_view *commands, size_t count, char **out) {
  gbnf_rule *rules = NULL;
  size_t nrules = 0, i;
  astools_buf b;
  astools_err e;

  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  astools_buf_init(&b);
  e = build_rules(commands,count,&rules,&nrules);
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

  free_rules(rules, nrules);
  if (e != ASTOOLS_OK) {
    astools_buf_free(&b);
    return e;
  }
  *out = astools_buf_detach(&b);
  if (!*out)
    return ASTOOLS_ERR_NOMEM;
  return ASTOOLS_OK;
}

astools_err astools_gbnf_render(astools_ctx *c, char **out) {
  if (!c || !out) return ASTOOLS_ERR_INVALID;
  astools_tool **tools = NULL; size_t n = 0, count = 0;
  astools_command_view *commands = NULL;
  os_rwlock_rdlock(&c->lock);
  astools_err e = astools_collect_tools(c,&tools,&n);
  for (size_t i = 0; i < n; i++) count += tools[i]->m->commands_len;
  if (e == ASTOOLS_OK && count) {
    commands = calloc(count,sizeof *commands);
    if (!commands) e = ASTOOLS_ERR_NOMEM;
  }
  size_t k = 0;
  for (size_t i = 0; e == ASTOOLS_OK && i < n; i++)
    for (size_t j = 0; j < tools[i]->m->commands_len; j++)
      commands[k++] = (astools_command_view){tools[i],&tools[i]->m->commands[j]};
  if (e == ASTOOLS_OK) e = astools_gbnf_commands(commands,count,out);
  os_rwlock_rdunlock(&c->lock);
  free(commands); free(tools); return e;
}
