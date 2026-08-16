/*
 * lockfile.c — astools.lock.xcdn load / check / approve.
 *
 * The lockfile is untrusted input: every shape is validated, hashes must be
 * exactly 32 bytes, and artifact paths taken from the file are only ever
 * joined under the package directory after a package-relative check.
 * astools_lockfile_check fails closed: whatever cannot be verified (I/O
 * errors, allocation failure, hostile rows) reports MISMATCH rather than OK.
 *
 * Relative lockfile paths are resolved by the caller (registry.c) against
 * the config dir; this module takes the path verbatim.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- shared predicates --------------------------------------------------- */

/* package-relative: contains a path separator, is not absolute (no
 * leading separator, no drive prefix), and has no ".." component. */
static bool pkg_relative(const char *p) {
  size_t i = 0;
  bool sep = false;
  if (!p || p[0] == '\0') return false;
  if (p[0] == '/' || p[0] == '\\') return false;
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':')
    return false; /* Windows drive prefix */
  if (os_path_is_abs(p)) return false;
  for (;;) {
    size_t n = 0;
    while (p[i + n] != '\0' && p[i + n] != '/' && p[i + n] != '\\') n++;
    if (n == 2 && p[i] == '.' && p[i + 1] == '.') return false;
    if (p[i + n] == '\0') break;
    sep = true;
    i += n + 1;
  }
  return sep;
}

/* ---- typed field access on parsed nodes ---------------------------------- */

static const char *obj_string(const xcdn_value_t *obj, const char *key) {
  xcdn_node_t *n = xcdn_object_get(obj, key);
  if (!n || !n->value || n->value->type != XCDN_VAL_STRING) return NULL;
  return n->value->data.string;
}

static const uint8_t *obj_bytes32(const xcdn_value_t *obj, const char *key) {
  xcdn_node_t *n = xcdn_object_get(obj, key);
  if (!n || !n->value || n->value->type != XCDN_VAL_BYTES) return NULL;
  if (n->value->data.bytes.len != 32 || !n->value->data.bytes.data) return NULL;
  return n->value->data.bytes.data;
}

/* ---- load ---------------------------------------------------------------- */

static void lf_entry_free(astools_lock_entry *en) {
  size_t i;
  if (!en) return;
  free(en->id);
  free(en->version);
  for (i = 0; i < en->artifacts_len; i++) free(en->artifacts[i].path);
  free(en->artifacts);
  memset(en, 0, sizeof *en);
}

static astools_err lf_parse_entry(const xcdn_value_t *obj,
                                  astools_lock_entry *en) {
  const char *id, *ver;
  const uint8_t *sha;
  xcdn_node_t *n;
  astools_err e = ASTOOLS_ERR_PARSE;
  size_t i;

  memset(en, 0, sizeof *en);
  if (!obj || obj->type != XCDN_VAL_OBJECT) return ASTOOLS_ERR_PARSE;
  id = obj_string(obj, "id");
  ver = obj_string(obj, "version");
  sha = obj_bytes32(obj, "manifest_sha256");
  if (!id || id[0] == '\0' || !ver || ver[0] == '\0' || !sha)
    return ASTOOLS_ERR_PARSE;
  en->id = astools_strdup(id);
  en->version = astools_strdup(ver);
  if (!en->id || !en->version) {
    e = ASTOOLS_ERR_NOMEM;
    goto fail;
  }
  memcpy(en->manifest_sha256, sha, 32);

  n = xcdn_object_get(obj, "approved_at");
  if (n) {
    if (!n->value || n->value->type != XCDN_VAL_DATETIME ||
        !n->value->data.string ||
        !astools_time_parse_rfc3339(n->value->data.string, &en->approved_at))
      goto fail;
  }

  n = xcdn_object_get(obj, "artifacts");
  if (n) {
    const xcdn_value_t *arr = n->value;
    size_t alen;
    if (!arr || arr->type != XCDN_VAL_ARRAY) goto fail;
    alen = xcdn_array_len(arr);
    if (alen > 0) {
      en->artifacts = calloc(alen, sizeof *en->artifacts);
      if (!en->artifacts) {
        e = ASTOOLS_ERR_NOMEM;
        goto fail;
      }
      for (i = 0; i < alen; i++) {
        xcdn_node_t *an = xcdn_array_get(arr, i);
        const xcdn_value_t *ao = an ? an->value : NULL;
        const char *ap;
        const uint8_t *ah;
        if (!ao || ao->type != XCDN_VAL_OBJECT) goto fail;
        ap = obj_string(ao, "path");
        ah = obj_bytes32(ao, "sha256");
        if (!ap || ap[0] == '\0' || !ah) goto fail;
        en->artifacts[i].path = astools_strdup(ap);
        if (!en->artifacts[i].path) {
          e = ASTOOLS_ERR_NOMEM;
          goto fail;
        }
        memcpy(en->artifacts[i].sha256, ah, 32);
        en->artifacts_len = i + 1;
      }
    }
  }
  return ASTOOLS_OK;

fail:
  lf_entry_free(en);
  return e;
}

astools_err astools_lockfile_load(const char *path, astools_lockfile *out) {
  char *text = NULL;
  size_t len = 0, n, i;
  xcdn_error_t xe;
  xcdn_document_t *doc;
  xcdn_node_t *root, *vn, *tn;
  const xcdn_value_t *rv, *tools;
  astools_err e;

  if (!path || !out) return ASTOOLS_ERR_INVALID;
  out->entries = NULL;
  out->len = 0;

  e = os_read_file(path, &text, &len);
  if (e == ASTOOLS_ERR_NOT_FOUND) return ASTOOLS_OK; /* missing => empty */
  if (e != ASTOOLS_OK) return e;

  doc = xcdn_parse_str(text, len, &xe);
  free(text);
  if (!doc) return ASTOOLS_ERR_PARSE;

  e = ASTOOLS_ERR_PARSE;
  root = xcdn_document_get(doc, 0);
  rv = root ? root->value : NULL;
  if (!root || !xcdn_node_has_tag(root, "astools_lock") || !rv ||
      rv->type != XCDN_VAL_OBJECT)
    goto out;
  vn = xcdn_object_get(rv, "version");
  if (!vn || !vn->value || vn->value->type != XCDN_VAL_INT ||
      vn->value->data.integer != 1)
    goto out;
  tn = xcdn_object_get(rv, "tools");
  if (!tn) {
    e = ASTOOLS_OK; /* no tools key => empty lockfile */
    goto out;
  }
  tools = tn->value;
  if (!tools || tools->type != XCDN_VAL_ARRAY) goto out;
  n = xcdn_array_len(tools);
  if (n > 0) {
    out->entries = calloc(n, sizeof *out->entries);
    if (!out->entries) {
      e = ASTOOLS_ERR_NOMEM;
      goto out;
    }
    for (i = 0; i < n; i++) {
      xcdn_node_t *en = xcdn_array_get(tools, i);
      e = lf_parse_entry(en ? en->value : NULL, &out->entries[i]);
      if (e != ASTOOLS_OK) goto out;
      out->len = i + 1;
    }
  }
  e = ASTOOLS_OK;

out:
  xcdn_document_free(doc);
  if (e != ASTOOLS_OK) astools_lockfile_free(out);
  return e;
}

void astools_lockfile_free(astools_lockfile *lf) {
  size_t i;
  if (!lf) return;
  for (i = 0; i < lf->len; i++) lf_entry_free(&lf->entries[i]);
  free(lf->entries);
  lf->entries = NULL;
  lf->len = 0;
}

/* ---- check --------------------------------------------------------------- */

/* 1 when the file exists as a regular file (stat errors count as absent:
 * the contract keys on "whose file exists"). */
static int lf_is_file(const char *path) {
  os_stat_info st;
  if (os_stat(path, &st) != ASTOOLS_OK) return 0;
  return st.type == OS_FT_FILE;
}

astools_lock_state astools_lockfile_check(const astools_lockfile *lf,
                                          const char *id, const char *version,
                                          const char *pkg_dir,
                                          const astools_manifest *m) {
  const astools_lock_entry *en = NULL;
  uint8_t h[32];
  size_t i, j;

  if (!lf || !id || !version || !pkg_dir || !m)
    return ASTOOLS_LOCK_MISMATCH; /* cannot verify: fail closed */

  for (i = 0; i < lf->len; i++) {
    if (strcmp(lf->entries[i].id, id) == 0 &&
        strcmp(lf->entries[i].version, version) == 0) {
      en = &lf->entries[i];
      break;
    }
  }
  if (!en) return ASTOOLS_LOCK_UNLISTED;

  /* the manifest.xcdn FILE BYTES, not the reparsed tree */
  {
    char *mp = os_path_join(pkg_dir, "manifest.xcdn");
    astools_err e;
    if (!mp) return ASTOOLS_LOCK_MISMATCH;
    e = astools_sha256_file(mp, h);
    free(mp);
    if (e != ASTOOLS_OK || memcmp(h, en->manifest_sha256, 32) != 0)
      return ASTOOLS_LOCK_MISMATCH;
  }

  /* every runtime entry whose package-relative argv[0] exists on disk must
   * have a matching artifacts[] row (path recorded as written) */
  for (i = 0; i < m->entries_len; i++) {
    const char *a0 = m->entries[i].argv_len > 0 ? m->entries[i].argv[0] : NULL;
    char *full;
    if (!a0 || !pkg_relative(a0)) continue;
    full = os_path_join(pkg_dir, a0);
    if (!full) return ASTOOLS_LOCK_MISMATCH;
    if (lf_is_file(full)) {
      const astools_lock_artifact *row = NULL;
      if (astools_sha256_file(full, h) != ASTOOLS_OK) {
        free(full);
        return ASTOOLS_LOCK_MISMATCH;
      }
      for (j = 0; j < en->artifacts_len; j++) {
        if (strcmp(en->artifacts[j].path, a0) == 0) {
          row = &en->artifacts[j];
          break;
        }
      }
      if (!row || memcmp(row->sha256, h, 32) != 0) {
        free(full);
        return ASTOOLS_LOCK_MISMATCH;
      }
    }
    free(full);
  }

  /* every listed artifact whose file exists must still match */
  for (i = 0; i < en->artifacts_len; i++) {
    const astools_lock_artifact *row = &en->artifacts[i];
    char *full;
    if (!pkg_relative(row->path))
      return ASTOOLS_LOCK_MISMATCH; /* hostile or corrupt row */
    full = os_path_join(pkg_dir, row->path);
    if (!full) return ASTOOLS_LOCK_MISMATCH;
    if (lf_is_file(full)) {
      if (astools_sha256_file(full, h) != ASTOOLS_OK ||
          memcmp(h, row->sha256, 32) != 0) {
        free(full);
        return ASTOOLS_LOCK_MISMATCH;
      }
    }
    free(full);
  }
  return ASTOOLS_LOCK_OK;
}

/* ---- approve ------------------------------------------------------------- */

static void lf_artifacts_free(astools_lock_artifact *v, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) free(v[i].path);
  free(v);
}

/* Hash every distinct package-relative entry argv[0] that exists as a
 * regular file under pkg_dir — the same set astools_lockfile_check
 * verifies. */
static astools_err lf_collect_artifacts(const char *pkg_dir,
                                        const astools_manifest *m,
                                        astools_lock_artifact **out,
                                        size_t *out_n) {
  astools_lock_artifact *v = NULL;
  size_t n = 0, i, j;
  astools_err e = ASTOOLS_OK;

  *out = NULL;
  *out_n = 0;
  for (i = 0; i < m->entries_len; i++) {
    const char *a0 = m->entries[i].argv_len > 0 ? m->entries[i].argv[0] : NULL;
    char *full;
    bool dup = false;
    if (!a0 || !pkg_relative(a0)) continue;
    for (j = 0; j < n; j++) {
      if (strcmp(v[j].path, a0) == 0) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    full = os_path_join(pkg_dir, a0);
    if (!full) {
      e = ASTOOLS_ERR_NOMEM;
      break;
    }
    if (lf_is_file(full)) {
      astools_lock_artifact *nv = realloc(v, (n + 1) * sizeof *v);
      if (!nv) {
        free(full);
        e = ASTOOLS_ERR_NOMEM;
        break;
      }
      v = nv;
      e = astools_sha256_file(full, v[n].sha256);
      if (e != ASTOOLS_OK) {
        free(full);
        break;
      }
      v[n].path = astools_strdup(a0);
      if (!v[n].path) {
        free(full);
        e = ASTOOLS_ERR_NOMEM;
        break;
      }
      n++;
    }
    free(full);
  }
  if (e != ASTOOLS_OK) {
    lf_artifacts_free(v, n);
    return e;
  }
  *out = v;
  *out_n = n;
  return ASTOOLS_OK;
}

/* The xcdn_* mutators grow their arrays without reporting allocation
 * failure, so the document is assembled with checked equivalents below;
 * only the checked helpers touch the AST array internals. */

/* Wrap v in a node and append it to arr; consumes v even on failure. */
static astools_err lf_pushv(xcdn_value_t *arr, xcdn_value_t *v) {
  xcdn_node_t *n;
  if (!v) return ASTOOLS_ERR_NOMEM;
  if (!arr || arr->type != XCDN_VAL_ARRAY) {
    xcdn_value_free(v);
    return ASTOOLS_ERR_INVALID;
  }
  n = xcdn_node_new(v);
  if (!n) {
    xcdn_value_free(v);
    return ASTOOLS_ERR_NOMEM;
  }
  if (arr->data.array.len >= arr->data.array.cap) {
    size_t nc = arr->data.array.cap ? arr->data.array.cap * 2 : 4;
    xcdn_node_t **np = realloc(arr->data.array.items, nc * sizeof *np);
    if (!np) {
      xcdn_node_free(n);
      return ASTOOLS_ERR_NOMEM;
    }
    arr->data.array.items = np;
    arr->data.array.cap = nc;
  }
  arr->data.array.items[arr->data.array.len++] = n;
  return ASTOOLS_OK;
}

/* Set obj[key] = v (keys here are unique by construction); consumes v. */
static astools_err lf_put(xcdn_value_t *obj, const char *key,
                          xcdn_value_t *v) {
  xcdn_node_t *n;
  char *k;
  if (!v) return ASTOOLS_ERR_NOMEM;
  if (!obj || obj->type != XCDN_VAL_OBJECT) {
    xcdn_value_free(v);
    return ASTOOLS_ERR_INVALID;
  }
  n = xcdn_node_new(v);
  if (!n) {
    xcdn_value_free(v);
    return ASTOOLS_ERR_NOMEM;
  }
  k = astools_strdup(key);
  if (!k) {
    xcdn_node_free(n);
    return ASTOOLS_ERR_NOMEM;
  }
  if (obj->data.object.len >= obj->data.object.cap) {
    size_t nc = obj->data.object.cap ? obj->data.object.cap * 2 : 4;
    xcdn_object_entry_t *np =
        realloc(obj->data.object.entries, nc * sizeof *np);
    if (!np) {
      free(k);
      xcdn_node_free(n);
      return ASTOOLS_ERR_NOMEM;
    }
    obj->data.object.entries = np;
    obj->data.object.cap = nc;
  }
  obj->data.object.entries[obj->data.object.len].key = k;
  obj->data.object.entries[obj->data.object.len].node = n;
  obj->data.object.len++;
  return ASTOOLS_OK;
}

static astools_err lf_add_tag(xcdn_node_t *n, const char *name) {
  char *nm = astools_strdup(name);
  if (!nm) return ASTOOLS_ERR_NOMEM;
  if (n->tags_len >= n->tags_cap) {
    size_t nc = n->tags_cap ? n->tags_cap * 2 : 4;
    xcdn_tag_t *nt = realloc(n->tags, nc * sizeof *nt);
    if (!nt) {
      free(nm);
      return ASTOOLS_ERR_NOMEM;
    }
    n->tags = nt;
    n->tags_cap = nc;
  }
  n->tags[n->tags_len].name = nm;
  n->tags_len++;
  return ASTOOLS_OK;
}

/* Constructors that may leave a NULL payload on OOM are re-checked here. */
static xcdn_value_t *lf_vstring(const char *s) {
  xcdn_value_t *v = xcdn_value_string(s);
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

static xcdn_value_t *lf_vdatetime(const char *s) {
  xcdn_value_t *v = xcdn_value_datetime(s);
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

static xcdn_value_t *lf_vbytes(const uint8_t *d, size_t n) {
  xcdn_value_t *v = xcdn_value_bytes(d, n);
  if (v && n > 0 && !v->data.bytes.data) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

static astools_err lf_emit_entry(xcdn_value_t *tools, const char *id,
                                 const char *version,
                                 const uint8_t msha[32],
                                 const astools_lock_artifact *arts,
                                 size_t arts_n, astools_time approved_at) {
  xcdn_value_t *obj = NULL, *arr = NULL;
  char stamp[32];
  size_t i;
  astools_err e;

  obj = xcdn_value_object();
  if (!obj) return ASTOOLS_ERR_NOMEM;
  e = lf_put(obj, "id", lf_vstring(id));
  if (e != ASTOOLS_OK) goto fail;
  e = lf_put(obj, "version", lf_vstring(version));
  if (e != ASTOOLS_OK) goto fail;
  e = lf_put(obj, "manifest_sha256", lf_vbytes(msha, 32));
  if (e != ASTOOLS_OK) goto fail;

  arr = xcdn_value_array();
  if (!arr) {
    e = ASTOOLS_ERR_NOMEM;
    goto fail;
  }
  for (i = 0; i < arts_n; i++) {
    xcdn_value_t *ao = xcdn_value_object();
    if (!ao) {
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
    e = lf_put(ao, "path", lf_vstring(arts[i].path));
    if (e == ASTOOLS_OK)
      e = lf_put(ao, "sha256", lf_vbytes(arts[i].sha256, 32));
    if (e != ASTOOLS_OK) {
      xcdn_value_free(ao);
      goto fail;
    }
    e = lf_pushv(arr, ao); /* consumes ao */
    if (e != ASTOOLS_OK) goto fail;
  }
  e = lf_put(obj, "artifacts", arr);
  arr = NULL; /* consumed */
  if (e != ASTOOLS_OK) goto fail;

  astools_time_format_rfc3339(approved_at, stamp);
  e = lf_put(obj, "approved_at", lf_vdatetime(stamp));
  if (e != ASTOOLS_OK) goto fail;

  e = lf_pushv(tools, obj); /* consumes obj */
  return e;

fail:
  if (arr) xcdn_value_free(arr);
  xcdn_value_free(obj);
  return e;
}

static astools_err lf_build_doc(const astools_lockfile *lf, const char *id,
                                const char *version, const uint8_t msha[32],
                                const astools_lock_artifact *arts,
                                size_t arts_n, astools_time now,
                                xcdn_document_t **out_doc) {
  xcdn_document_t *doc;
  xcdn_value_t *root = NULL, *tools = NULL;
  xcdn_node_t *rn = NULL;
  bool replaced = false;
  size_t i;
  astools_err e = ASTOOLS_ERR_NOMEM;

  *out_doc = NULL;
  doc = xcdn_document_new();
  root = xcdn_value_object();
  tools = xcdn_value_array();
  if (!doc || !root || !tools) goto fail;

  e = lf_put(root, "version", xcdn_value_int(1));
  if (e != ASTOOLS_OK) goto fail;

  /* other entries stay untouched and keep their position; the target
   * (id,version) entry is replaced in place, or appended when new */
  for (i = 0; i < lf->len; i++) {
    const astools_lock_entry *en = &lf->entries[i];
    if (!replaced && strcmp(en->id, id) == 0 &&
        strcmp(en->version, version) == 0) {
      e = lf_emit_entry(tools, id, version, msha, arts, arts_n, now);
      replaced = true;
    } else {
      e = lf_emit_entry(tools, en->id, en->version, en->manifest_sha256,
                        en->artifacts, en->artifacts_len, en->approved_at);
    }
    if (e != ASTOOLS_OK) goto fail;
  }
  if (!replaced) {
    e = lf_emit_entry(tools, id, version, msha, arts, arts_n, now);
    if (e != ASTOOLS_OK) goto fail;
  }

  e = lf_put(root, "tools", tools);
  tools = NULL; /* consumed */
  if (e != ASTOOLS_OK) goto fail;

  rn = xcdn_node_new(root);
  if (!rn) {
    e = ASTOOLS_ERR_NOMEM;
    goto fail;
  }
  root = NULL; /* owned by rn */
  e = lf_add_tag(rn, "astools_lock");
  if (e != ASTOOLS_OK) goto fail;

  /* attach with checked growth (xcdn_document_push_value grows unchecked) */
  doc->values = malloc(sizeof *doc->values);
  if (!doc->values) {
    e = ASTOOLS_ERR_NOMEM;
    goto fail;
  }
  doc->values[0] = rn;
  doc->values_len = 1;
  doc->values_cap = 1;
  *out_doc = doc;
  return ASTOOLS_OK;

fail:
  if (rn) xcdn_node_free(rn);
  if (root) xcdn_value_free(root);
  if (tools) xcdn_value_free(tools);
  if (doc) xcdn_document_free(doc);
  return e;
}

astools_err astools_lockfile_approve(const char *path, const char *id,
                                     const char *version, const char *pkg_dir,
                                     const astools_manifest *m,
                                     astools_time now) {
  astools_lockfile lf = {NULL, 0};
  astools_lock_artifact *arts = NULL;
  size_t arts_n = 0, plen;
  uint8_t msha[32];
  xcdn_document_t *doc = NULL;
  char *mp = NULL, *tmp = NULL, *text = NULL;
  astools_err e;

  if (!path || !id || !version || !pkg_dir || !m) return ASTOOLS_ERR_INVALID;

  /* a corrupt existing lockfile is never silently clobbered */
  e = astools_lockfile_load(path, &lf);
  if (e != ASTOOLS_OK) return e;

  mp = os_path_join(pkg_dir, "manifest.xcdn");
  if (!mp) {
    e = ASTOOLS_ERR_NOMEM;
    goto out;
  }
  e = astools_sha256_file(mp, msha);
  if (e != ASTOOLS_OK) goto out;

  e = lf_collect_artifacts(pkg_dir, m, &arts, &arts_n);
  if (e != ASTOOLS_OK) goto out;

  e = lf_build_doc(&lf, id, version, msha, arts, arts_n, now, &doc);
  if (e != ASTOOLS_OK) goto out;

  text = xcdn_to_string_pretty(doc);
  if (!text) {
    e = ASTOOLS_ERR_NOMEM;
    goto out;
  }

  plen = strlen(path);
  tmp = malloc(plen + 5);
  if (!tmp) {
    e = ASTOOLS_ERR_NOMEM;
    goto out;
  }
  memcpy(tmp, path, plen);
  memcpy(tmp + plen, ".tmp", 5);

  e = os_write_file(tmp, text, strlen(text));
  if (e == ASTOOLS_OK) e = os_file_replace(tmp, path); /* atomic */
  if (e != ASTOOLS_OK) (void)os_remove_file(tmp);

out:
  free(mp);
  free(tmp);
  free(text);
  if (doc) xcdn_document_free(doc);
  lf_artifacts_free(arts, arts_n);
  astools_lockfile_free(&lf);
  return e;
}
