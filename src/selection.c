/* One resolved registry order for prompts, GBNF and JSON Schema. */
#include "astools_internal.h"
#include <stdlib.h>
#include <string.h>

static bool tool_listed(const astools_tool *t) {
  return t != NULL && t->available && t->enabled && t->m != NULL &&
         t->m->id != NULL;
}

/* Release beats pre-release; then higher SemVer. Ties keep the incumbent
 * (first root wins). */
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

astools_err astools_collect_tools(astools_ctx *c, astools_tool ***out_list,
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

