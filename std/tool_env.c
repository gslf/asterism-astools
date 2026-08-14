/*
 * tool_env.c — the `env` standard tool (§8.7): env.get / env.list.
 *
 * Only variables named in the request's grants.env array are visible.
 * An ungranted variable and a granted-but-unset one produce the exact
 * same shape ({name, present: false}) so a caller cannot probe which
 * grants exist (no oracle). Pure ISO C: works on every platform.
 */

#include "sdk.h"
#include <stdlib.h>
#include <string.h>

static int cmp_names(const void *a, const void *b) {
  const char *const *x = (const char *const *)a;
  const char *const *y = (const char *const *)b;
  return strcmp(*x, *y);
}

/* Collect granted env names: sorted, deduplicated, borrowed from r->doc.
 * Non-string entries in a hostile grants array are ignored. Returns 0 on
 * success (*out may be NULL when *out_n is 0), -1 on OOM. */
static int granted_names(const astd_req *r, const char ***out,
                         size_t *out_n) {
  const xcdn_value_t *arr = NULL;
  const char **v;
  size_t n, i, w;

  *out = NULL;
  *out_n = 0;
  if (r->grants) {
    const xcdn_node_t *node = xcdn_object_get(r->grants, "env");
    if (node && node->value && node->value->type == XCDN_VAL_ARRAY)
      arr = node->value;
  }
  if (!arr) return 0;
  n = xcdn_array_len(arr);
  if (n == 0) return 0;
  if (n > SIZE_MAX / sizeof *v) return -1;
  v = malloc(n * sizeof *v);
  if (!v) return -1;
  w = 0;
  for (i = 0; i < n; i++) {
    const xcdn_node_t *it = xcdn_array_get(arr, i);
    if (it && it->value && it->value->type == XCDN_VAL_STRING &&
        it->value->data.string)
      v[w++] = it->value->data.string;
  }
  if (w == 0) {
    free((void *)v);
    return 0;
  }
  qsort((void *)v, w, sizeof *v, cmp_names);
  {
    size_t o = 1;
    for (i = 1; i < w; i++)
      if (strcmp(v[i], v[o - 1]) != 0) v[o++] = v[i];
    w = o;
  }
  *out = v;
  *out_n = w;
  return 0;
}

static int cmd_get(astd_req *r) {
  const char *name = astd_arg_str(r, "name", NULL);
  const char **names = NULL;
  size_t n = 0, i;
  const char *value = NULL;
  xcdn_value_t *res;
  int present;

  if (!name) {
    astd_fail(r, "astools/invalid-args", "name is required");
    return 0;
  }
  if (granted_names(r, &names, &n) != 0) goto oom;
  for (i = 0; i < n; i++) {
    if (strcmp(names[i], name) == 0) {
      value = getenv(name);
      break;
    }
  }
  free((void *)names);
  names = NULL;
  present = value != NULL;

  res = xcdn_value_object();
  if (!res) goto oom;
  if (astd_set_str(res, "name", name) != 0 ||
      astd_set_bool(res, "present", present) != 0 ||
      (present && astd_set_str(res, "value", value) != 0)) {
    xcdn_value_free(res);
    goto oom;
  }
  astd_ok(r, res);
  return 0;

oom:
  free((void *)names);
  astd_fail(r, "astools/protocol", "out of memory");
  return 0;
}

static int cmd_list(astd_req *r) {
  const char **names = NULL;
  size_t n = 0, i;
  xcdn_value_t *res = NULL, *vars = NULL;

  if (granted_names(r, &names, &n) != 0) goto oom;
  res = xcdn_value_object();
  vars = xcdn_value_array();
  if (!res || !vars) goto oom;
  for (i = 0; i < n; i++) {
    const char *val = getenv(names[i]);
    xcdn_value_t *e;
    int ok = 0;
    if (!val) continue; /* granted but unset: omitted from list */
    e = xcdn_value_object();
    if (!e) goto oom;
    do {
      if (astd_set_str(e, "name", names[i]) != 0) break;
      if (astd_set_str(e, "value", val) != 0) break;
      ok = 1;
    } while (0);
    if (!ok) {
      xcdn_value_free(e);
      goto oom;
    }
    if (astd_arr_push_val(vars, e) != 0) goto oom; /* e consumed */
  }
  free((void *)names);
  names = NULL;
  if (astd_set_val(res, "vars", vars) != 0) {
    vars = NULL;
    goto oom;
  }
  vars = NULL;
  astd_ok(r, res);
  return 0;

oom:
  free((void *)names);
  xcdn_value_free(vars);
  xcdn_value_free(res);
  astd_fail(r, "astools/protocol", "out of memory");
  return 0;
}

int astd_tool_env(astd_req *r) {
  const char *cmd = r ? r->command : NULL;
  if (!cmd) {
    astd_fail(r, "astools/invalid-args", "missing command");
    return 0;
  }
  if (strcmp(cmd, "get") == 0) return cmd_get(r);
  if (strcmp(cmd, "list") == 0) return cmd_list(r);
  astd_fail(r, "astools/invalid-args", "unknown env command \"%s\"", cmd);
  return 0;
}
