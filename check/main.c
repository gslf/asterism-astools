/*
 * main.c — astools-check CLI (SPEC §A2.4): manifest schema + lint, catalog
 * and grammar rendering, lockfile approval, Agent Plugin lint.
 *
 * Exit codes: manifest mode returns the number of lint errors (2 on read/
 * parse failure); --plugin-lint returns the number of violations; the
 * registry modes return 0/1; usage errors return 2. Counts are clamped to
 * 255 so they survive the exit-status truncation.
 */

#include "astools_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * --plugin-lint parses two small JSON files. astools-check links only
 * libastools, which is xCDN-only by design; the project's one JSON codec
 * lives in mcp/json.c (built into astools-mcp). Reusing it by textual
 * inclusion keeps the lint dependency-honest: one in-house strict codec,
 * no second hand-rolled parser, no build-system change.
 */
#include "../mcp/json.c"

/* ---- shared helpers ------------------------------------------------------ */

static void chk_usage(FILE *to) {
  fputs(
      "usage: astools-check <manifest-or-package-dir>\n"
      "       astools-check --catalog --root <dir> [--workspace <w>]\n"
      "                     [--config <file>]\n"
      "                     [--level index|summary|full] [--budget N]\n"
      "       astools-check --grammar --root <dir> [--workspace <w>]\n"
      "                     [--config <file>]\n"
      "       astools-check --approve <ref> --root <dir> [--config <file>]\n"
      "                     [--workspace <w>]\n"
      "       astools-check --plugin-lint <dir>\n"
      "       astools-check --help | --version\n",
      to);
}

/* Clamp a finding count into the representable exit-status range. */
static int chk_exit_count(int n) {
  if (n < 0) return 0;
  return n > 255 ? 255 : n;
}

/* Print text ensuring a trailing newline. */
static void chk_puts(const char *text) {
  size_t n;
  if (!text) return;
  n = strlen(text);
  if (n == 0) return;
  fputs(text, stdout);
  if (text[n - 1] != '\n') fputc('\n', stdout);
}

static void chk_report(astools_ctx *c, astools_err e, const char *what) {
  const char *msg = c ? astools_last_error(c) : NULL;
  fprintf(stderr, "error: %s failed: %s\n", what,
          (msg && msg[0] != '\0') ? msg : astools_err_name(e));
}

typedef struct {
  const char *root;
  const char *workspace;
  const char *config;
  const char *level;
  const char *budget;
} chk_opts;

static int chk_parse_opts(int argc, char **argv, chk_opts *o, int allow_level,
                          int allow_config) {
  int i;
  memset(o, 0, sizeof *o);
  for (i = 0; i < argc; i++) {
    const char *a = argv[i];
    const char **slot = NULL;
    if (strcmp(a, "--root") == 0)
      slot = &o->root;
    else if (strcmp(a, "--workspace") == 0)
      slot = &o->workspace;
    else if (allow_config && strcmp(a, "--config") == 0)
      slot = &o->config;
    else if (allow_level && strcmp(a, "--level") == 0)
      slot = &o->level;
    else if (allow_level && strcmp(a, "--budget") == 0)
      slot = &o->budget;
    if (!slot) {
      fprintf(stderr, "error: unknown option '%s'\n", a);
      return -1;
    }
    if (i + 1 >= argc) {
      fprintf(stderr, "error: option '%s' needs a value\n", a);
      return -1;
    }
    *slot = argv[++i];
  }
  if (!o->root) {
    fprintf(stderr, "error: --root is required\n");
    return -1;
  }
  return 0;
}

static int chk_open(const chk_opts *o, astools_ctx **out) {
  astools_open_params p;
  const char *paths[2];
  astools_err e;
  memset(&p, 0, sizeof p);
  paths[0] = o->root;
  paths[1] = NULL;
  p.registry_paths = paths;
  p.config_path = o->config;
  p.workspace_root = o->workspace;
  *out = NULL;
  e = astools_open(&p, out);
  if (e != ASTOOLS_OK) {
    chk_report(*out, e, "open");
    if (*out) astools_close(*out);
    *out = NULL;
    return -1;
  }
  return 0;
}

/* ---- manifest check ------------------------------------------------------ */

static int cmd_manifest(const char *arg) {
  os_stat_info st;
  char *mpath;
  char *text = NULL;
  size_t len = 0;
  char *err_msg = NULL;
  astools_manifest *m;
  astools_buf b;
  int errs;

  if (os_stat(arg, &st) == ASTOOLS_OK && st.type == OS_FT_DIR)
    mpath = os_path_join(arg, "manifest.xcdn");
  else
    mpath = astools_strdup(arg);
  if (!mpath) {
    fprintf(stderr, "error: out of memory\n");
    return 2;
  }
  if (os_read_file(mpath, &text, &len) != ASTOOLS_OK) {
    printf("error: cannot read %s\n", mpath);
    free(mpath);
    return 2;
  }
  m = astools_manifest_parse(text, len, NULL, &err_msg);
  free(text);
  if (!m) {
    printf("error: %s\n", err_msg ? err_msg : "manifest rejected");
    free(err_msg);
    free(mpath);
    return 2;
  }
  free(mpath);

  astools_buf_init(&b);
  errs = astools_manifest_lint(m, &b);
  if (b.data && b.len > 0) {
    fputs(b.data, stdout);
    if (b.data[b.len - 1] != '\n') fputc('\n', stdout);
  }
  if (errs == 0)
    printf("ok: %s %s, %zu commands\n", m->id, m->version, m->commands_len);
  astools_buf_free(&b);
  astools_manifest_free(m);
  return chk_exit_count(errs);
}

/* ---- catalog / grammar / approve ---------------------------------------- */

static int cmd_catalog(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  astools_catalog_level lvl = ASTOOLS_CATALOG_SUMMARY;
  size_t budget = 0;
  char *text = NULL;
  astools_err e;

  if (chk_parse_opts(argc, argv, &o, 1, 1) != 0) return 2;
  if (o.level) {
    if (strcmp(o.level, "index") == 0)
      lvl = ASTOOLS_CATALOG_INDEX;
    else if (strcmp(o.level, "summary") == 0)
      lvl = ASTOOLS_CATALOG_SUMMARY;
    else if (strcmp(o.level, "full") == 0)
      lvl = ASTOOLS_CATALOG_FULL;
    else {
      fprintf(stderr, "error: --level must be index, summary or full\n");
      return 2;
    }
  }
  if (o.budget) {
    char *end = NULL;
    long v = strtol(o.budget, &end, 10);
    if (end == o.budget || *end != '\0' || v < 0) {
      fprintf(stderr, "error: --budget must be a non-negative integer\n");
      return 2;
    }
    budget = (size_t)v;
  }
  if (chk_open(&o, &c) != 0) return 1;
  e = astools_catalog(c, lvl, budget, &text);
  if (e != ASTOOLS_OK) {
    chk_report(c, e, "catalog");
    astools_close(c);
    return 1;
  }
  chk_puts(text);
  astools_free(text);
  astools_close(c);
  return 0;
}

static int cmd_grammar(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  char *text = NULL;
  astools_err e;

  if (chk_parse_opts(argc, argv, &o, 0, 1) != 0) return 2;
  if (chk_open(&o, &c) != 0) return 1;
  e = astools_grammar_export(c, &text);
  if (e != ASTOOLS_OK) {
    chk_report(c, e, "grammar");
    astools_close(c);
    return 1;
  }
  chk_puts(text);
  astools_free(text);
  astools_close(c);
  return 0;
}

static int cmd_approve(int argc, char **argv) {
  chk_opts o;
  astools_ctx *c;
  const char *ref;
  astools_err e;

  if (argc < 1 || argv[0][0] == '-') {
    fprintf(stderr, "error: --approve needs a tool ref\n");
    return 2;
  }
  ref = argv[0];
  if (chk_parse_opts(argc - 1, argv + 1, &o, 0, 1) != 0) return 2;
  if (chk_open(&o, &c) != 0) return 1;
  e = astools_tool_approve(c, ref);
  if (e != ASTOOLS_OK) {
    chk_report(c, e, "approve");
    astools_close(c);
    return 1;
  }
  printf("approved: %s\n", ref);
  astools_close(c);
  return 0;
}

/* ---- plugin lint (SPEC §A2.3) ------------------------------------------- */

static void pl_bad(int *n, const char *clause, const char *fmt, ...) {
  va_list ap;
  printf("violation [%s]: ", clause);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fputc('\n', stdout);
  if (*n < INT_MAX) (*n)++;
}

/* Load <dir>/<name> as a JSON object; NULL (with a violation recorded)
 * when missing, malformed, or not an object. */
static jx_value *pl_load(const char *dir, const char *name,
                         const char *clause, int *n) {
  char *path = os_path_join(dir, name);
  char *text = NULL;
  size_t len = 0;
  jx_value *v = NULL;

  if (!path) {
    fprintf(stderr, "error: out of memory\n");
    return NULL;
  }
  if (os_read_file(path, &text, &len) != ASTOOLS_OK) {
    pl_bad(n, clause, "%s: missing or unreadable", name);
    free(path);
    return NULL;
  }
  free(path);
  if (jx_parse(text, len, &v) != 0 || !v) {
    pl_bad(n, clause, "%s: not valid RFC 8259 JSON", name);
    free(text);
    return NULL;
  }
  free(text);
  if (jx_typeof(v) != JX_OBJECT) {
    pl_bad(n, clause, "%s: top level must be a JSON object", name);
    jx_free(v);
    return NULL;
  }
  return v;
}

static void pl_expect_str(const jx_value *obj, const char *key,
                          const char *want, const char *where,
                          const char *clause, int *n) {
  const char *s = jx_string_value(jx_object_get(obj, key));
  if (!s)
    pl_bad(n, clause, "%s: \"%s\" must be the string \"%s\"", where, key,
           want);
  else if (strcmp(s, want) != 0)
    pl_bad(n, clause, "%s: \"%s\" is \"%s\", must be \"%s\"", where, key, s,
           want);
}

static void pl_check_plugin(const char *dir, int *n) {
  static const char clause[] = "SPEC A2.3.2";
  jx_value *root = pl_load(dir, "plugin.json", clause, n);
  const char *s;

  if (!root) return;
  pl_expect_str(root, "$schema",
                "https://agent-plugins.org/schemas/1.0.0/plugin.schema.json",
                "plugin.json", clause, n);
  s = jx_string_value(jx_object_get(root, "name"));
  if (!s)
    pl_bad(n, clause, "plugin.json: \"name\" must be a string");
  else if (!astools_slug_valid(s))
    pl_bad(n, clause,
           "plugin.json: \"name\" must match [a-z0-9][a-z0-9-]*");
  else if (strcmp(s, "astools") != 0)
    pl_bad(n, clause, "plugin.json: \"name\" must be \"astools\"");
  s = jx_string_value(jx_object_get(root, "version"));
  if (!s || s[0] == '\0')
    pl_bad(n, clause, "plugin.json: \"version\" must be a non-empty string");
  pl_expect_str(root, "license", "MIT", "plugin.json", clause, n);
  jx_free(root);
}

static void pl_check_mcp(const char *dir, int *n) {
  static const char clause[] = "SPEC A2.3.3";
  jx_value *root = pl_load(dir, "mcp.json", clause, n);
  const jx_value *servers, *srv, *args;
  const char *s;

  if (!root) return;
  pl_expect_str(root, "$schema",
                "https://agent-plugins.org/schemas/1.0.0/mcp.schema.json",
                "mcp.json", clause, n);
  servers = jx_object_get(root, "mcpServers");
  if (jx_typeof(servers) != JX_OBJECT) {
    pl_bad(n, clause, "mcp.json: \"mcpServers\" must be an object");
    jx_free(root);
    return;
  }
  if (jx_object_count(servers) != 1)
    pl_bad(n, clause,
           "mcp.json: exactly one server must be declared, found %zu",
           jx_object_count(servers));
  srv = jx_object_get(servers, "astools");
  if (!srv) {
    pl_bad(n, clause, "mcp.json: the server must be named \"astools\"");
    jx_free(root);
    return;
  }
  if (jx_typeof(srv) != JX_OBJECT) {
    pl_bad(n, clause, "mcp.json: server \"astools\" must be an object");
    jx_free(root);
    return;
  }
  pl_expect_str(srv, "type", "stdio", "mcp.json server", clause, n);
  s = jx_string_value(jx_object_get(srv, "command"));
  if (!s || strcmp(s, "astools-mcp") != 0)
    pl_bad(n, clause,
           "mcp.json server: \"command\" must be exactly \"astools-mcp\" "
           "(bare token, PATH strategy D17)");
  args = jx_object_get(srv, "args");
  {
    const char *a0 = jx_string_value(jx_array_at(args, 0));
    const char *a1 = jx_string_value(jx_array_at(args, 1));
    if (jx_typeof(args) != JX_ARRAY || jx_array_len(args) != 2 || !a0 ||
        !a1 || strcmp(a0, "--config") != 0 ||
        strcmp(a1, "${PLUGIN_DATA}/config.xcdn") != 0)
      pl_bad(n, clause,
             "mcp.json server: \"args\" must be exactly [\"--config\", "
             "\"${PLUGIN_DATA}/config.xcdn\"]");
  }
  pl_expect_str(srv, "cwd", "${PLUGIN_DATA}", "mcp.json server", clause, n);
  if (jx_object_get(srv, "env") != NULL)
    pl_bad(n, clause,
           "mcp.json server: must not declare \"env\" (the host owns the "
           "child environment)");
  jx_free(root);
}

/* One line of text: length without the \r?\n terminator; *next points at
 * the following line (or the NUL). */
static size_t pl_line(const char *p, const char **next) {
  size_t i = 0;
  while (p[i] != '\0' && p[i] != '\n') i++;
  *next = (p[i] == '\n') ? p + i + 1 : p + i;
  if (i > 0 && p[i - 1] == '\r') i--;
  return i;
}

static int pl_line_eq(const char *line, size_t len, const char *want) {
  size_t wl = strlen(want);
  return len == wl && memcmp(line, want, wl) == 0;
}

/* Accepts "name: astools" with any blank padding around tokens. */
static int pl_is_name_astools(const char *line, size_t len) {
  size_t i = 0, j;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (len - i < 4 || memcmp(line + i, "name", 4) != 0) return 0;
  i += 4;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  if (i >= len || line[i] != ':') return 0;
  i++;
  while (i < len && (line[i] == ' ' || line[i] == '\t')) i++;
  j = len;
  while (j > i && (line[j - 1] == ' ' || line[j - 1] == '\t')) j--;
  return (j - i == 7) && memcmp(line + i, "astools", 7) == 0;
}

static void pl_check_skill(const char *dir, int *n) {
  static const char clause[] = "SPEC A2.3.1";
  static const char rel[] = "skills/astools/SKILL.md";
  char *path = os_path_join(dir, rel);
  char *text = NULL;
  size_t len = 0;

  if (!path) {
    fprintf(stderr, "error: out of memory\n");
    return;
  }
  if (os_read_file(path, &text, &len) != ASTOOLS_OK) {
    pl_bad(n, clause, "%s: missing or unreadable", rel);
    free(path);
    return;
  }
  free(path);
  {
    const char *p = text;
    const char *next;
    size_t ln = pl_line(p, &next);
    if (!pl_line_eq(p, ln, "---")) {
      pl_bad(n, clause,
             "%s: must start with a \"---\" YAML frontmatter line", rel);
    } else {
      int closed = 0, name_ok = 0;
      p = next;
      while (*p != '\0') {
        ln = pl_line(p, &next);
        if (pl_line_eq(p, ln, "---")) {
          closed = 1;
          break;
        }
        if (pl_is_name_astools(p, ln)) name_ok = 1;
        p = next;
      }
      if (!closed)
        pl_bad(n, clause, "%s: frontmatter never closed with \"---\"", rel);
      if (!name_ok)
        pl_bad(n, clause, "%s: frontmatter must contain \"name: astools\"",
               rel);
    }
  }
  free(text);
}

static int cmd_plugin_lint(int argc, char **argv) {
  int n = 0;
  if (argc != 1 || argv[0][0] == '-') {
    fprintf(stderr, "error: --plugin-lint needs a plugin directory\n");
    return 2;
  }
  pl_check_plugin(argv[0], &n);
  pl_check_mcp(argv[0], &n);
  pl_check_skill(argv[0], &n);
  if (n == 0) printf("ok: plugin layout conforms to SPEC A2.3\n");
  return chk_exit_count(n);
}

/* ---- entry --------------------------------------------------------------- */

int main(int argc, char **argv) {
  const char *a1;
  if (argc < 2) {
    chk_usage(stderr);
    return 2;
  }
  a1 = argv[1];
  if (strcmp(a1, "--help") == 0 || strcmp(a1, "-h") == 0) {
    chk_usage(stdout);
    return 0;
  }
  if (strcmp(a1, "--version") == 0) {
    printf("astools-check %s\n", ASTOOLS_VERSION);
    return 0;
  }
  if (strcmp(a1, "--catalog") == 0) return cmd_catalog(argc - 2, argv + 2);
  if (strcmp(a1, "--grammar") == 0) return cmd_grammar(argc - 2, argv + 2);
  if (strcmp(a1, "--approve") == 0) return cmd_approve(argc - 2, argv + 2);
  if (strcmp(a1, "--plugin-lint") == 0)
    return cmd_plugin_lint(argc - 2, argv + 2);
  if (a1[0] == '-') {
    fprintf(stderr, "error: unknown option '%s'\n", a1);
    chk_usage(stderr);
    return 2;
  }
  return cmd_manifest(a1);
}
