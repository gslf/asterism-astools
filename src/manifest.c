/*
 * manifest.c — #astools_tool parsing, schema validation, lint and canonical
 * re-serialization.
 *
 * Manifests are hostile input: every field is type-checked before use, the
 * first schema violation aborts the parse with a precise "manifest: ..."
 * message, and every error path funnels through astools_manifest_free.
 *
 * The xcdn mutators (xcdn_array_push & co.) grow their backing arrays
 * without reporting allocation failure, which can write out of bounds
 * under OOM. All programmatic AST construction here therefore goes through
 * local checked variants that fail loudly instead.
 */

#include "astools_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ---- error reporting ---------------------------------------------------- */

/* First error wins; later calls are no-ops. NULL err_msg tolerated. */
static void set_err(char **err_msg, const char *fmt, ...) {
  va_list ap;
  int need;
  char *p;
  if (!err_msg || *err_msg) return;
  va_start(ap, fmt);
  need = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (need < 0) return;
  p = malloc((size_t)need + 1);
  if (!p) return;
  va_start(ap, fmt);
  vsnprintf(p, (size_t)need + 1, fmt, ap);
  va_end(ap);
  *err_msg = p;
}

static void set_oom(char **err_msg) {
  set_err(err_msg, "manifest: out of memory");
}

/* ---- node inspection ---------------------------------------------------- */

static bool node_is_type(const xcdn_node_t *n, xcdn_value_type_t t) {
  return n != NULL && n->value != NULL && n->value->type == t;
}

/* Strictly XCDN_VAL_STRING (not datetime/uuid/... which also carry text). */
static const char *node_str(const xcdn_node_t *n) {
  if (!node_is_type(n, XCDN_VAL_STRING)) return NULL;
  return n->value->data.string;
}

/* Optional bool field: absent leaves *out untouched; wrong type fails. */
static bool get_opt_bool(const xcdn_value_t *obj, const char *key, bool *out) {
  const xcdn_node_t *n = xcdn_object_get(obj, key);
  if (!n) return true;
  if (!node_is_type(n, XCDN_VAL_BOOL)) return false;
  *out = n->value->data.boolean;
  return true;
}

/* Optional string field: absent leaves *out NULL; wrong type or OOM fails
 * (oom flag distinguishes the two for messaging). */
static bool get_opt_string(const xcdn_value_t *obj, const char *key,
                           char **out, bool *oom) {
  const xcdn_node_t *n = xcdn_object_get(obj, key);
  const char *s;
  *oom = false;
  if (!n) return true;
  s = node_str(n);
  if (!s) return false;
  *out = astools_strdup(s);
  if (!*out) {
    *oom = true;
    return false;
  }
  return true;
}

/* Duration node -> positive milliseconds. */
static bool dur_node_ms(const xcdn_node_t *n, int64_t *out_ms) {
  const char *s;
  int64_t secs;
  if (!node_is_type(n, XCDN_VAL_DURATION)) return false;
  s = n->value->data.string;
  if (!s || !astools_duration_parse(s, &secs)) return false;
  if (secs <= 0 || secs > INT64_MAX / 1000) return false;
  *out_ms = secs * 1000;
  return true;
}

/* Command param names: [a-z][a-z0-9_]{0,31}. */
static bool param_name_valid(const char *s) {
  size_t i;
  if (!s || s[0] < 'a' || s[0] > 'z') return false;
  for (i = 1; s[i] != '\0'; i++) {
    char ch = s[i];
    if (i > 31) return false;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_'))
      return false;
  }
  return true;
}

/* Environment variable names: [A-Za-z_][A-Za-z0-9_]*. */
static bool env_name_valid(const char *s) {
  size_t i;
  char ch;
  if (!s) return false;
  ch = s[0];
  if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_'))
    return false;
  for (i = 1; s[i] != '\0'; i++) {
    ch = s[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '_'))
      return false;
  }
  return true;
}

static bool os_name_valid(const char *s) {
  return s && (strcmp(s, "linux") == 0 || strcmp(s, "macos") == 0 ||
               strcmp(s, "windows") == 0);
}

static bool arch_name_valid(const char *s) {
  return s && (strcmp(s, "x86_64") == 0 || strcmp(s, "arm64") == 0);
}

static int access_from_string(const char *s) {
  if (!s) return 0;
  if (strcmp(s, "read") == 0) return ASTOOLS_ACCESS_READ;
  if (strcmp(s, "write") == 0) return ASTOOLS_ACCESS_WRITE;
  if (strcmp(s, "read-write") == 0) return ASTOOLS_ACCESS_RW;
  return 0;
}

static const char *access_to_string(int access) {
  switch (access) {
    case ASTOOLS_ACCESS_READ: return "read";
    case ASTOOLS_ACCESS_WRITE: return "write";
    case ASTOOLS_ACCESS_RW: return "read-write";
    default: return NULL;
  }
}

/* Replace every "${workspace}" occurrence; plain strdup when workspace is
 * NULL or nothing matches. NULL on OOM. */
static char *expand_workspace(const char *path, const char *workspace) {
  static const char needle[] = "${workspace}";
  const size_t nlen = sizeof(needle) - 1;
  const char *p = path, *hit;
  astools_buf b;
  if (!workspace || strstr(path, needle) == NULL)
    return astools_strdup(path);
  astools_buf_init(&b);
  while ((hit = strstr(p, needle)) != NULL) {
    if (astools_buf_append(&b, p, (size_t)(hit - p)) != ASTOOLS_OK ||
        astools_buf_appends(&b, workspace) != ASTOOLS_OK) {
      astools_buf_free(&b);
      return NULL;
    }
    p = hit + nlen;
  }
  if (astools_buf_appends(&b, p) != ASTOOLS_OK) {
    astools_buf_free(&b);
    return NULL;
  }
  return astools_buf_detach(&b);
}

/* ---- checked xcdn builders ---------------------------------------------- */

static bool arr_push_checked(xcdn_value_t *arr, xcdn_node_t *node) {
  if (!arr || arr->type != XCDN_VAL_ARRAY || !node) return false;
  if (arr->data.array.len >= arr->data.array.cap) {
    size_t ncap = arr->data.array.cap ? arr->data.array.cap * 2 : 4;
    xcdn_node_t **p;
    if (ncap > SIZE_MAX / sizeof(*p)) return false;
    p = realloc(arr->data.array.items, ncap * sizeof(*p));
    if (!p) return false;
    arr->data.array.items = p;
    arr->data.array.cap = ncap;
  }
  arr->data.array.items[arr->data.array.len++] = node;
  return true;
}

/* Append-only insert (callers never repeat keys; clones must preserve the
 * source order and any duplicate keys verbatim). */
static bool obj_append_checked(xcdn_value_t *obj, const char *key,
                               xcdn_node_t *node) {
  char *k;
  if (!obj || obj->type != XCDN_VAL_OBJECT || !key || !node) return false;
  if (obj->data.object.len >= obj->data.object.cap) {
    size_t ncap = obj->data.object.cap ? obj->data.object.cap * 2 : 4;
    xcdn_object_entry_t *p;
    if (ncap > SIZE_MAX / sizeof(*p)) return false;
    p = realloc(obj->data.object.entries, ncap * sizeof(*p));
    if (!p) return false;
    obj->data.object.entries = p;
    obj->data.object.cap = ncap;
  }
  k = astools_strdup(key);
  if (!k) return false;
  obj->data.object.entries[obj->data.object.len].key = k;
  obj->data.object.entries[obj->data.object.len].node = node;
  obj->data.object.len++;
  return true;
}

static bool node_add_tag_checked(xcdn_node_t *node, const char *name) {
  char *dup;
  if (!node || !name) return false;
  if (node->tags_len >= node->tags_cap) {
    size_t ncap = node->tags_cap ? node->tags_cap * 2 : 4;
    xcdn_tag_t *p;
    if (ncap > SIZE_MAX / sizeof(*p)) return false;
    p = realloc(node->tags, ncap * sizeof(*p));
    if (!p) return false;
    node->tags = p;
    node->tags_cap = ncap;
  }
  dup = astools_strdup(name);
  if (!dup) return false;
  node->tags[node->tags_len].name = dup;
  node->tags_len++;
  return true;
}

static bool node_add_annotation_checked(xcdn_node_t *node, const char *name) {
  char *dup;
  if (!node || !name) return false;
  if (node->annotations_len >= node->annotations_cap) {
    size_t ncap = node->annotations_cap ? node->annotations_cap * 2 : 4;
    xcdn_annotation_t *p;
    if (ncap > SIZE_MAX / sizeof(*p)) return false;
    p = realloc(node->annotations, ncap * sizeof(*p));
    if (!p) return false;
    node->annotations = p;
    node->annotations_cap = ncap;
  }
  dup = astools_strdup(name);
  if (!dup) return false;
  memset(&node->annotations[node->annotations_len], 0,
         sizeof(node->annotations[0]));
  node->annotations[node->annotations_len].name = dup;
  node->annotations_len++;
  return true;
}

static bool ann_push_arg_checked(xcdn_annotation_t *ann, xcdn_value_t *val) {
  if (!ann || !val) return false;
  if (ann->args_len >= ann->args_cap) {
    size_t ncap = ann->args_cap ? ann->args_cap * 2 : 4;
    xcdn_value_t **p;
    if (ncap > SIZE_MAX / sizeof(*p)) return false;
    p = realloc(ann->args, ncap * sizeof(*p));
    if (!p) return false;
    ann->args = p;
    ann->args_cap = ncap;
  }
  ann->args[ann->args_len++] = val;
  return true;
}

static bool doc_push_checked(xcdn_document_t *doc, xcdn_node_t *node) {
  if (!doc || !node) return false;
  if (doc->values_len >= doc->values_cap) {
    size_t ncap = doc->values_cap ? doc->values_cap * 2 : 4;
    xcdn_node_t **p;
    if (ncap > SIZE_MAX / sizeof(*p)) return false;
    p = realloc(doc->values, ncap * sizeof(*p));
    if (!p) return false;
    doc->values = p;
    doc->values_cap = ncap;
  }
  doc->values[doc->values_len++] = node;
  return true;
}

/* ---- deep clone --------------------------------------------------------- */

static xcdn_node_t *clone_node(const xcdn_node_t *n);

static xcdn_value_t *clone_value(const xcdn_value_t *v) {
  xcdn_value_t *out = NULL;
  size_t i;
  if (!v) return NULL;
  switch (v->type) {
    case XCDN_VAL_NULL:
      return xcdn_value_null();
    case XCDN_VAL_BOOL:
      return xcdn_value_bool(v->data.boolean);
    case XCDN_VAL_INT:
      return xcdn_value_int(v->data.integer);
    case XCDN_VAL_FLOAT:
      return xcdn_value_float(v->data.floating);
    case XCDN_VAL_DECIMAL:
    case XCDN_VAL_STRING:
    case XCDN_VAL_DATETIME:
    case XCDN_VAL_DURATION:
    case XCDN_VAL_UUID: {
      const char *s = v->data.string;
      switch (v->type) {
        case XCDN_VAL_DECIMAL: out = xcdn_value_decimal(s); break;
        case XCDN_VAL_STRING: out = xcdn_value_string(s); break;
        case XCDN_VAL_DATETIME: out = xcdn_value_datetime(s); break;
        case XCDN_VAL_DURATION: out = xcdn_value_duration(s); break;
        default: out = xcdn_value_uuid(s); break;
      }
      /* the constructors do not report strdup failure */
      if (out && s && !out->data.string) {
        xcdn_value_free(out);
        return NULL;
      }
      return out;
    }
    case XCDN_VAL_BYTES:
      if (v->data.bytes.len > 0 && !v->data.bytes.data) return NULL;
      out = xcdn_value_bytes(v->data.bytes.data, v->data.bytes.len);
      if (out && v->data.bytes.len > 0 && !out->data.bytes.data) {
        xcdn_value_free(out);
        return NULL;
      }
      return out;
    case XCDN_VAL_ARRAY:
      out = xcdn_value_array();
      if (!out) return NULL;
      for (i = 0; i < v->data.array.len; i++) {
        xcdn_node_t *item = clone_node(v->data.array.items[i]);
        if (!item || !arr_push_checked(out, item)) {
          xcdn_node_free(item);
          xcdn_value_free(out);
          return NULL;
        }
      }
      return out;
    case XCDN_VAL_OBJECT:
      out = xcdn_value_object();
      if (!out) return NULL;
      for (i = 0; i < v->data.object.len; i++) {
        xcdn_node_t *item = clone_node(v->data.object.entries[i].node);
        if (!item ||
            !obj_append_checked(out, v->data.object.entries[i].key, item)) {
          xcdn_node_free(item);
          xcdn_value_free(out);
          return NULL;
        }
      }
      return out;
    default:
      return NULL;
  }
}

static xcdn_node_t *clone_node(const xcdn_node_t *n) {
  xcdn_node_t *out;
  size_t i, j;
  if (!n) return NULL;
  out = xcdn_node_new(NULL);
  if (!out) return NULL;
  for (i = 0; i < n->tags_len; i++)
    if (!node_add_tag_checked(out, n->tags[i].name)) goto fail;
  for (i = 0; i < n->annotations_len; i++) {
    if (!node_add_annotation_checked(out, n->annotations[i].name)) goto fail;
    for (j = 0; j < n->annotations[i].args_len; j++) {
      xcdn_value_t *av = clone_value(n->annotations[i].args[j]);
      if (!av) goto fail;
      if (!ann_push_arg_checked(&out->annotations[i], av)) {
        xcdn_value_free(av);
        goto fail;
      }
    }
  }
  if (n->value) {
    out->value = clone_value(n->value);
    if (!out->value) goto fail;
  }
  return out;
fail:
  xcdn_node_free(out);
  return NULL;
}

/* ---- free --------------------------------------------------------------- */

static void free_strv(char **v, size_t n) {
  size_t i;
  if (!v) return;
  for (i = 0; i < n; i++) free(v[i]);
  free(v);
}

static void free_cmd_members(astools_cmd *c) {
  size_t i;
  if (!c) return;
  free(c->name);
  free(c->summary);
  free(c->description);
  if (c->params) {
    for (i = 0; i < c->params_len; i++)
      astools_param_free_fields(&c->params[i]);
    free(c->params);
  }
  if (c->returns_type) astools_type_free(c->returns_type);
  free(c->returns_desc);
  if (c->errors) {
    for (i = 0; i < c->errors_len; i++) {
      free(c->errors[i].code);
      free(c->errors[i].description);
    }
    free(c->errors);
  }
  if (c->examples) {
    for (i = 0; i < c->examples_len; i++) {
      free(c->examples[i].title);
      free(c->examples[i].note);
      xcdn_node_free(c->examples[i].call);
      xcdn_node_free(c->examples[i].result);
    }
    free(c->examples);
  }
}

void astools_manifest_free(astools_manifest *m) {
  size_t i;
  if (!m) return;
  free(m->id);
  free(m->version);
  free(m->title);
  free(m->summary);
  free(m->description);
  free_strv(m->platforms, m->platforms_len);
  if (m->entries) {
    for (i = 0; i < m->entries_len; i++) {
      free(m->entries[i].os);
      free(m->entries[i].arch);
      free_strv(m->entries[i].argv, m->entries[i].argv_len);
    }
    free(m->entries);
  }
  if (m->perms.fs) {
    for (i = 0; i < m->perms.fs_len; i++) free(m->perms.fs[i].path);
    free(m->perms.fs);
  }
  free_strv(m->perms.env, m->perms.env_len);
  if (m->commands) {
    for (i = 0; i < m->commands_len; i++) free_cmd_members(&m->commands[i]);
    free(m->commands);
  }
  free(m);
}

/* ---- parse: header fields ----------------------------------------------- */

static bool parse_header(astools_manifest *m, const xcdn_value_t *obj,
                         char **err_msg) {
  const xcdn_node_t *n;
  const char *s;
  astools_semver sv;
  bool oom;

  n = xcdn_object_get(obj, "manifest_version");
  if (!n || !node_is_type(n, XCDN_VAL_INT)) {
    set_err(err_msg, "manifest: manifest_version must be an int");
    return false;
  }
  if (n->value->data.integer != 1) {
    set_err(err_msg, "manifest: unsupported manifest_version %lld (expected 1)",
            (long long)n->value->data.integer);
    return false;
  }
  m->manifest_version = 1;

  s = node_str(xcdn_object_get(obj, "id"));
  if (!s || !astools_slug_valid(s) || strlen(s) > 32) {
    set_err(err_msg,
            "manifest: id must be a slug [a-z0-9][a-z0-9-]* of at most "
            "32 chars");
    return false;
  }
  m->id = astools_strdup(s);
  if (!m->id) goto oom;

  s = node_str(xcdn_object_get(obj, "version"));
  if (!s || !astools_semver_parse(s, &sv)) {
    set_err(err_msg, "manifest: version must be a SemVer 2.0.0 string");
    return false;
  }
  astools_semver_free(&sv);
  m->version = astools_strdup(s);
  if (!m->version) goto oom;

  s = node_str(xcdn_object_get(obj, "title"));
  if (!s || astools_str_blank(s)) {
    set_err(err_msg, "manifest: title must be a non-blank string");
    return false;
  }
  m->title = astools_strdup(s);
  if (!m->title) goto oom;

  s = node_str(xcdn_object_get(obj, "summary"));
  if (!s || astools_str_blank(s) || strlen(s) > 80 || strchr(s, '\n') ||
      strchr(s, '\r')) {
    set_err(err_msg,
            "manifest: summary must be a single non-blank line of at most "
            "80 bytes");
    return false;
  }
  m->summary = astools_strdup(s);
  if (!m->summary) goto oom;

  if (!get_opt_string(obj, "description", &m->description, &oom)) {
    if (oom) goto oom;
    set_err(err_msg, "manifest: description must be a string");
    return false;
  }

  /* kind defaults to "executable" (the least-privileged interpretation);
   * "library" must be declared explicitly. */
  m->kind = ASTOOLS_KIND_EXECUTABLE;
  n = xcdn_object_get(obj, "kind");
  if (n) {
    s = node_str(n);
    if (s && strcmp(s, "executable") == 0) {
      m->kind = ASTOOLS_KIND_EXECUTABLE;
    } else if (s && strcmp(s, "library") == 0) {
      m->kind = ASTOOLS_KIND_LIBRARY;
    } else {
      set_err(err_msg,
              "manifest: kind must be \"executable\" or \"library\"");
      return false;
    }
  }
  return true;

oom:
  set_oom(err_msg);
  return false;
}

/* ---- parse: platforms --------------------------------------------------- */

static bool parse_platforms(astools_manifest *m, const xcdn_value_t *obj,
                            char **err_msg) {
  const xcdn_node_t *n = xcdn_object_get(obj, "platforms");
  size_t i, len;

  if (!n) {
    static const char *all[3] = {"linux", "macos", "windows"};
    m->platforms = calloc(3, sizeof(char *));
    if (!m->platforms) goto oom;
    for (i = 0; i < 3; i++) {
      m->platforms[i] = astools_strdup(all[i]);
      if (!m->platforms[i]) {
        m->platforms_len = i;
        goto oom;
      }
    }
    m->platforms_len = 3;
    return true;
  }

  if (!node_is_type(n, XCDN_VAL_ARRAY)) {
    set_err(err_msg, "manifest: platforms must be an array");
    return false;
  }
  len = xcdn_array_len(n->value);
  m->platforms = calloc(len ? len : 1, sizeof(char *));
  if (!m->platforms) goto oom;
  for (i = 0; i < len; i++) {
    const char *s = node_str(xcdn_array_get(n->value, i));
    if (!os_name_valid(s)) {
      set_err(err_msg,
              "manifest: platforms[%zu] must be \"linux\", \"macos\" or "
              "\"windows\"",
              i);
      return false;
    }
    m->platforms[i] = astools_strdup(s);
    if (!m->platforms[i]) goto oom;
    m->platforms_len = i + 1;
  }
  return true;

oom:
  set_oom(err_msg);
  return false;
}

/* ---- parse: runtime ----------------------------------------------------- */

static bool parse_entry(astools_manifest *m, const xcdn_node_t *item,
                        astools_entry *e, size_t idx, char **err_msg) {
  const xcdn_value_t *obj;
  const xcdn_node_t *n;
  const char *s;
  size_t i, len;

  if (!node_is_type(item, XCDN_VAL_OBJECT)) {
    set_err(err_msg, "manifest: runtime.entry[%zu] must be an object", idx);
    return false;
  }
  obj = item->value;

  s = node_str(xcdn_object_get(obj, "os"));
  if (!os_name_valid(s)) {
    set_err(err_msg,
            "manifest: runtime.entry[%zu].os must be \"linux\", \"macos\" "
            "or \"windows\"",
            idx);
    return false;
  }
  e->os = astools_strdup(s);
  if (!e->os) goto oom;

  s = node_str(xcdn_object_get(obj, "arch"));
  if (!arch_name_valid(s)) {
    set_err(err_msg,
            "manifest: runtime.entry[%zu].arch must be \"x86_64\" or "
            "\"arm64\"",
            idx);
    return false;
  }
  e->arch = astools_strdup(s);
  if (!e->arch) goto oom;

  for (i = 0; i < m->entries_len; i++) {
    if (strcmp(m->entries[i].os, e->os) == 0 &&
        strcmp(m->entries[i].arch, e->arch) == 0) {
      set_err(err_msg, "manifest: runtime.entry: duplicate entry for (%s, %s)",
              e->os, e->arch);
      return false;
    }
  }

  n = xcdn_object_get(obj, "argv");
  if (!node_is_type(n, XCDN_VAL_ARRAY) || xcdn_array_len(n->value) == 0) {
    set_err(err_msg,
            "manifest: runtime.entry[%zu].argv must be a non-empty array "
            "of strings",
            idx);
    return false;
  }
  len = xcdn_array_len(n->value);
  e->argv = calloc(len, sizeof(char *));
  if (!e->argv) goto oom;
  for (i = 0; i < len; i++) {
    s = node_str(xcdn_array_get(n->value, i));
    if (!s || (i == 0 && s[0] == '\0')) {
      set_err(err_msg,
              "manifest: runtime.entry[%zu].argv must be a non-empty array "
              "of strings (argv[0] non-empty)",
              idx);
      return false;
    }
    e->argv[i] = astools_strdup(s);
    if (!e->argv[i]) goto oom;
    e->argv_len = i + 1;
  }
  return true;

oom:
  set_oom(err_msg);
  return false;
}

static bool parse_runtime(astools_manifest *m, const xcdn_value_t *obj,
                          char **err_msg) {
  const xcdn_node_t *rt = xcdn_object_get(obj, "runtime");
  const xcdn_node_t *n;
  const xcdn_value_t *r;
  size_t i, len;

  if (!rt) return true; /* all defaults */
  if (!node_is_type(rt, XCDN_VAL_OBJECT)) {
    set_err(err_msg, "manifest: runtime must be an object");
    return false;
  }
  r = rt->value;

  n = xcdn_object_get(r, "mode");
  if (n) {
    const char *s = node_str(n);
    if (s && strcmp(s, "oneshot") == 0) {
      m->mode = ASTOOLS_MODE_ONESHOT;
    } else if (s && strcmp(s, "persistent") == 0) {
      m->mode = ASTOOLS_MODE_PERSISTENT;
    } else {
      set_err(err_msg,
              "manifest: runtime.mode must be \"oneshot\" or \"persistent\"");
      return false;
    }
  }

  n = xcdn_object_get(r, "entry");
  if (n) {
    if (!node_is_type(n, XCDN_VAL_ARRAY)) {
      set_err(err_msg, "manifest: runtime.entry must be an array");
      return false;
    }
    len = xcdn_array_len(n->value);
    if (len > 0) {
      m->entries = calloc(len, sizeof(astools_entry));
      if (!m->entries) {
        set_oom(err_msg);
        return false;
      }
      for (i = 0; i < len; i++) {
        /* entries_len counts fully or partially parsed slots so free
         * releases what parse_entry allocated before failing */
        m->entries_len = i;
        if (!parse_entry(m, xcdn_array_get(n->value, i), &m->entries[i], i,
                         err_msg)) {
          m->entries_len = i + 1;
          return false;
        }
      }
      m->entries_len = len;
    }
  }

  n = xcdn_object_get(r, "parallel");
  if (n) {
    if (!node_is_type(n, XCDN_VAL_INT) || n->value->data.integer < 1) {
      set_err(err_msg, "manifest: runtime.parallel must be an int >= 1");
      return false;
    }
    m->parallel = n->value->data.integer;
  }

  n = xcdn_object_get(r, "idle_timeout");
  if (n && !dur_node_ms(n, &m->idle_timeout_ms)) {
    set_err(err_msg,
            "manifest: runtime.idle_timeout must be a positive duration");
    return false;
  }

  n = xcdn_object_get(r, "startup_timeout");
  if (n && !dur_node_ms(n, &m->startup_timeout_ms)) {
    set_err(err_msg,
            "manifest: runtime.startup_timeout must be a positive duration");
    return false;
  }
  return true;
}

/* ---- parse: permissions ------------------------------------------------- */

static bool parse_permissions(astools_manifest *m, const xcdn_value_t *obj,
                              const char *workspace, char **err_msg) {
  const xcdn_node_t *pn = xcdn_object_get(obj, "permissions");
  const xcdn_node_t *n;
  const xcdn_value_t *p;
  size_t i, len;

  if (!pn) return true;
  if (!node_is_type(pn, XCDN_VAL_OBJECT)) {
    set_err(err_msg, "manifest: permissions must be an object");
    return false;
  }
  p = pn->value;

  n = xcdn_object_get(p, "fs");
  if (n) {
    if (!node_is_type(n, XCDN_VAL_ARRAY)) {
      set_err(err_msg, "manifest: permissions.fs must be an array");
      return false;
    }
    len = xcdn_array_len(n->value);
    if (len > 0) {
      m->perms.fs = calloc(len, sizeof(astools_fs_perm));
      if (!m->perms.fs) goto oom;
      for (i = 0; i < len; i++) {
        const xcdn_node_t *item = xcdn_array_get(n->value, i);
        const char *path, *acc;
        int a;
        if (!node_is_type(item, XCDN_VAL_OBJECT)) {
          set_err(err_msg, "manifest: permissions.fs[%zu] must be an object",
                  i);
          return false;
        }
        path = node_str(xcdn_object_get(item->value, "path"));
        if (!path || path[0] == '\0') {
          set_err(err_msg,
                  "manifest: permissions.fs[%zu].path must be a non-empty "
                  "string",
                  i);
          return false;
        }
        acc = node_str(xcdn_object_get(item->value, "access"));
        a = access_from_string(acc);
        if (a == 0) {
          set_err(err_msg,
                  "manifest: permissions.fs[%zu].access must be \"read\", "
                  "\"write\" or \"read-write\"",
                  i);
          return false;
        }
        m->perms.fs[i].path = expand_workspace(path, workspace);
        if (!m->perms.fs[i].path) goto oom;
        m->perms.fs[i].access = a;
        m->perms.fs_len = i + 1;
      }
    }
  }

  if (!get_opt_bool(p, "net", &m->perms.net)) {
    set_err(err_msg, "manifest: permissions.net must be a bool");
    return false;
  }
  if (!get_opt_bool(p, "proc", &m->perms.proc)) {
    set_err(err_msg, "manifest: permissions.proc must be a bool");
    return false;
  }

  n = xcdn_object_get(p, "env");
  if (n) {
    if (!node_is_type(n, XCDN_VAL_ARRAY)) {
      set_err(err_msg, "manifest: permissions.env must be an array");
      return false;
    }
    len = xcdn_array_len(n->value);
    if (len > 0) {
      m->perms.env = calloc(len, sizeof(char *));
      if (!m->perms.env) goto oom;
      for (i = 0; i < len; i++) {
        const char *s = node_str(xcdn_array_get(n->value, i));
        if (!env_name_valid(s)) {
          set_err(err_msg,
                  "manifest: permissions.env[%zu] must be an environment "
                  "variable name [A-Za-z_][A-Za-z0-9_]*",
                  i);
          return false;
        }
        m->perms.env[i] = astools_strdup(s);
        if (!m->perms.env[i]) goto oom;
        m->perms.env_len = i + 1;
      }
    }
  }
  return true;

oom:
  set_oom(err_msg);
  return false;
}

/* ---- parse: params ------------------------------------------------------ */

static bool parse_params(const astools_manifest *m, astools_cmd *c,
                         const xcdn_node_t *pn, char **err_msg) {
  size_t i, j, len;
  char tbuf[256];

  (void)m;
  if (!node_is_type(pn, XCDN_VAL_ARRAY)) {
    set_err(err_msg, "manifest: command '%s': params must be an array",
            c->name);
    return false;
  }
  len = xcdn_array_len(pn->value);
  if (len == 0) return true;
  c->params = calloc(len, sizeof(astools_param));
  if (!c->params) goto oom;
  /* zeroed slots are safe for astools_param_free_fields */
  c->params_len = len;

  for (i = 0; i < len; i++) {
    const xcdn_node_t *item = xcdn_array_get(pn->value, i);
    astools_param *par = &c->params[i];
    const xcdn_value_t *po;
    const xcdn_node_t *n;
    const char *s;
    bool oomf;

    if (!node_is_type(item, XCDN_VAL_OBJECT)) {
      set_err(err_msg, "manifest: command '%s': params[%zu] must be an object",
              c->name, i);
      return false;
    }
    po = item->value;

    s = node_str(xcdn_object_get(po, "name"));
    if (!param_name_valid(s)) {
      set_err(err_msg,
              "manifest: command '%s': params[%zu].name must match "
              "[a-z][a-z0-9_]{0,31}",
              c->name, i);
      return false;
    }
    for (j = 0; j < i; j++) {
      if (c->params[j].name && strcmp(c->params[j].name, s) == 0) {
        set_err(err_msg, "manifest: command '%s': duplicate param '%s'",
                c->name, s);
        return false;
      }
    }
    par->name = astools_strdup(s);
    if (!par->name) goto oom;

    n = xcdn_object_get(po, "type");
    if (!n) {
      set_err(err_msg, "manifest: command '%s': param '%s': missing type",
              c->name, par->name);
      return false;
    }
    tbuf[0] = '\0';
    par->type = astools_type_parse(n, tbuf, sizeof(tbuf));
    if (!par->type) {
      set_err(err_msg, "manifest: command '%s': param '%s': %s", c->name,
              par->name, tbuf[0] ? tbuf : "invalid type");
      return false;
    }

    if (!get_opt_bool(po, "required", &par->required)) {
      set_err(err_msg,
              "manifest: command '%s': param '%s': required must be a bool",
              c->name, par->name);
      return false;
    }

    n = xcdn_object_get(po, "default");
    if (n) {
      astools_err e;
      if (par->required) {
        set_err(err_msg,
                "manifest: command '%s': param '%s': default is only "
                "allowed on optional params",
                c->name, par->name);
        return false;
      }
      par->dflt = clone_node(n);
      if (!par->dflt) goto oom;
      tbuf[0] = '\0';
      e = astools_type_check(par->type, par->dflt, par->name, tbuf,
                             sizeof(tbuf));
      if (e != ASTOOLS_OK) {
        set_err(err_msg, "manifest: command '%s': param '%s': default: %s",
                c->name, par->name,
                tbuf[0] ? tbuf : "does not match the declared type");
        return false;
      }
    }

    if (!get_opt_string(po, "description", &par->description, &oomf)) {
      if (oomf) goto oom;
      set_err(err_msg,
              "manifest: command '%s': param '%s': description must be a "
              "string",
              c->name, par->name);
      return false;
    }
  }
  return true;

oom:
  set_oom(err_msg);
  return false;
}

/* ---- parse: command ----------------------------------------------------- */

static bool parse_command(const astools_manifest *m, astools_cmd *c,
                          const xcdn_node_t *item, size_t idx,
                          const astools_cmd *cmds, size_t prev_n,
                          char **err_msg) {
  const xcdn_value_t *co;
  const xcdn_node_t *n;
  const char *s;
  size_t i, len;
  char tbuf[256];
  bool oomf;

  if (!node_is_type(item, XCDN_VAL_OBJECT)) {
    set_err(err_msg, "manifest: commands[%zu] must be an object", idx);
    return false;
  }
  co = item->value;

  s = node_str(xcdn_object_get(co, "name"));
  if (!s || !astools_slug_valid(s)) {
    set_err(err_msg, "manifest: commands[%zu].name must be a slug", idx);
    return false;
  }
  for (i = 0; i < prev_n; i++) {
    if (cmds[i].name && strcmp(cmds[i].name, s) == 0) {
      set_err(err_msg, "manifest: duplicate command name '%s'", s);
      return false;
    }
  }
  c->name = astools_strdup(s);
  if (!c->name) goto oom;

  s = node_str(xcdn_object_get(co, "summary"));
  if (!s || astools_str_blank(s)) {
    set_err(err_msg,
            "manifest: command '%s': summary must be a non-blank string",
            c->name);
    return false;
  }
  c->summary = astools_strdup(s);
  if (!c->summary) goto oom;

  if (!get_opt_string(co, "description", &c->description, &oomf)) {
    if (oomf) goto oom;
    set_err(err_msg, "manifest: command '%s': description must be a string",
            c->name);
    return false;
  }

  n = xcdn_object_get(co, "annotations");
  if (n) {
    static const char *keys[4] = {"read_only", "destructive", "idempotent",
                                  "long_running"};
    bool *flags[4];
    flags[0] = &c->read_only;
    flags[1] = &c->destructive;
    flags[2] = &c->idempotent;
    flags[3] = &c->long_running;
    if (!node_is_type(n, XCDN_VAL_OBJECT)) {
      set_err(err_msg, "manifest: command '%s': annotations must be an object",
              c->name);
      return false;
    }
    for (i = 0; i < 4; i++) {
      if (!get_opt_bool(n->value, keys[i], flags[i])) {
        set_err(err_msg,
                "manifest: command '%s': annotations.%s must be a bool",
                c->name, keys[i]);
        return false;
      }
    }
  }

  if (!get_opt_bool(co, "deprecated", &c->deprecated)) {
    set_err(err_msg, "manifest: command '%s': deprecated must be a bool",
            c->name);
    return false;
  }

  n = xcdn_object_get(co, "timeout");
  if (n && !dur_node_ms(n, &c->timeout_ms)) {
    set_err(err_msg,
            "manifest: command '%s': timeout must be a positive duration",
            c->name);
    return false;
  }

  n = xcdn_object_get(co, "params");
  if (n && !parse_params(m, c, n, err_msg)) return false;

  n = xcdn_object_get(co, "returns");
  if (n) {
    const xcdn_node_t *tn;
    if (!node_is_type(n, XCDN_VAL_OBJECT)) {
      set_err(err_msg, "manifest: command '%s': returns must be an object",
              c->name);
      return false;
    }
    tn = xcdn_object_get(n->value, "type");
    if (!tn) {
      set_err(err_msg, "manifest: command '%s': returns.type is required",
              c->name);
      return false;
    }
    tbuf[0] = '\0';
    c->returns_type = astools_type_parse(tn, tbuf, sizeof(tbuf));
    if (!c->returns_type) {
      set_err(err_msg, "manifest: command '%s': returns.type: %s", c->name,
              tbuf[0] ? tbuf : "invalid type");
      return false;
    }
    if (!get_opt_string(n->value, "description", &c->returns_desc, &oomf)) {
      if (oomf) goto oom;
      set_err(err_msg,
              "manifest: command '%s': returns.description must be a string",
              c->name);
      return false;
    }
  }

  n = xcdn_object_get(co, "errors");
  if (n) {
    if (!node_is_type(n, XCDN_VAL_ARRAY)) {
      set_err(err_msg, "manifest: command '%s': errors must be an array",
              c->name);
      return false;
    }
    len = xcdn_array_len(n->value);
    if (len > 0) {
      size_t idlen = strlen(m->id);
      c->errors = calloc(len, sizeof(astools_errdecl));
      if (!c->errors) goto oom;
      for (i = 0; i < len; i++) {
        const xcdn_node_t *en = xcdn_array_get(n->value, i);
        const char *code;
        if (!node_is_type(en, XCDN_VAL_OBJECT)) {
          set_err(err_msg,
                  "manifest: command '%s': errors[%zu] must be an object",
                  c->name, i);
          return false;
        }
        code = node_str(xcdn_object_get(en->value, "code"));
        if (!code || strncmp(code, m->id, idlen) != 0 ||
            code[idlen] != '/' || !astools_slug_valid(code + idlen + 1)) {
          set_err(err_msg,
                  "manifest: command '%s': errors[%zu].code must be "
                  "\"%s/<slug>\"",
                  c->name, i, m->id);
          return false;
        }
        c->errors[i].code = astools_strdup(code);
        if (!c->errors[i].code) goto oom;
        c->errors_len = i + 1;
        if (!get_opt_string(en->value, "description",
                            &c->errors[i].description, &oomf)) {
          if (oomf) goto oom;
          set_err(err_msg,
                  "manifest: command '%s': errors[%zu].description must be "
                  "a string",
                  c->name, i);
          return false;
        }
      }
    }
  }

  n = xcdn_object_get(co, "examples");
  if (n) {
    if (!node_is_type(n, XCDN_VAL_ARRAY)) {
      set_err(err_msg, "manifest: command '%s': examples must be an array",
              c->name);
      return false;
    }
    len = xcdn_array_len(n->value);
    if (len > 0) {
      c->examples = calloc(len, sizeof(astools_example));
      if (!c->examples) goto oom;
      c->examples_len = len; /* zeroed slots are free-safe */
      for (i = 0; i < len; i++) {
        const xcdn_node_t *ex = xcdn_array_get(n->value, i);
        const xcdn_node_t *cn;
        if (!node_is_type(ex, XCDN_VAL_OBJECT)) {
          set_err(err_msg,
                  "manifest: command '%s': examples[%zu] must be an object",
                  c->name, i);
          return false;
        }
        if (!get_opt_string(ex->value, "title", &c->examples[i].title,
                            &oomf)) {
          if (oomf) goto oom;
          set_err(err_msg,
                  "manifest: command '%s': examples[%zu].title must be a "
                  "string",
                  c->name, i);
          return false;
        }
        cn = xcdn_object_get(ex->value, "call");
        if (!node_is_type(cn, XCDN_VAL_OBJECT)) {
          set_err(err_msg,
                  "manifest: command '%s': examples[%zu].call must be an "
                  "object",
                  c->name, i);
          return false;
        }
        c->examples[i].call = clone_node(cn);
        if (!c->examples[i].call) goto oom;
        cn = xcdn_object_get(ex->value, "result");
        if (cn) {
          c->examples[i].result = clone_node(cn);
          if (!c->examples[i].result) goto oom;
        }
        if (!get_opt_string(ex->value, "note", &c->examples[i].note, &oomf)) {
          if (oomf) goto oom;
          set_err(err_msg,
                  "manifest: command '%s': examples[%zu].note must be a "
                  "string",
                  c->name, i);
          return false;
        }
      }
    }
  }
  return true;

oom:
  set_oom(err_msg);
  return false;
}

static bool parse_commands(astools_manifest *m, const xcdn_value_t *obj,
                           char **err_msg) {
  const xcdn_node_t *n = xcdn_object_get(obj, "commands");
  size_t i, len;

  if (!n || !node_is_type(n, XCDN_VAL_ARRAY) ||
      xcdn_array_len(n->value) == 0) {
    set_err(err_msg, "manifest: commands must be a non-empty array");
    return false;
  }
  len = xcdn_array_len(n->value);
  m->commands = calloc(len, sizeof(astools_cmd));
  if (!m->commands) {
    set_oom(err_msg);
    return false;
  }
  m->commands_len = len; /* zeroed slots are free-safe */
  for (i = 0; i < len; i++) {
    if (!parse_command(m, &m->commands[i], xcdn_array_get(n->value, i), i,
                       m->commands, i, err_msg))
      return false;
  }
  return true;
}

/* ---- parse entry point -------------------------------------------------- */

/* Unknown top-level keys (tags, license, authors, homepage, keywords, and
 * any forward-compat extension within manifest_version 1) are ignored. */
astools_manifest *astools_manifest_parse(const char *text, size_t len,
                                         const char *workspace,
                                         char **err_msg) {
  xcdn_error_t xerr;
  xcdn_document_t *doc = NULL;
  const xcdn_node_t *root;
  astools_manifest *m = NULL;

  if (err_msg) *err_msg = NULL;
  if (!text) {
    set_err(err_msg, "manifest: no input");
    return NULL;
  }

  doc = xcdn_parse_str(text, len, &xerr);
  if (!doc) {
    set_err(err_msg, "manifest: parse error at line %zu, col %zu: %s",
            xerr.span.line, xerr.span.column, xerr.message);
    return NULL;
  }

  root = xcdn_document_get(doc, 0);
  if (!root || !xcdn_node_has_tag(root, "astools_tool") ||
      !node_is_type(root, XCDN_VAL_OBJECT)) {
    set_err(err_msg,
            "manifest: first top-level value must be a #astools_tool object");
    xcdn_document_free(doc);
    return NULL;
  }

  m = calloc(1, sizeof(*m));
  if (!m) {
    set_oom(err_msg);
    xcdn_document_free(doc);
    return NULL;
  }
  /* defaults */
  m->mode = ASTOOLS_MODE_ONESHOT;
  m->parallel = 1;
  m->idle_timeout_ms = 120000;
  m->startup_timeout_ms = 10000;

  if (!parse_header(m, root->value, err_msg) ||
      !parse_platforms(m, root->value, err_msg) ||
      !parse_runtime(m, root->value, err_msg) ||
      !parse_permissions(m, root->value, workspace, err_msg) ||
      !parse_commands(m, root->value, err_msg)) {
    astools_manifest_free(m);
    xcdn_document_free(doc);
    return NULL;
  }

  xcdn_document_free(doc);
  return m;
}

/* ---- lookup ------------------------------------------------------------- */

const astools_cmd *astools_manifest_cmd(const astools_manifest *m,
                                        const char *name) {
  size_t i;
  if (!m || !name) return NULL;
  for (i = 0; i < m->commands_len; i++) {
    if (m->commands[i].name && strcmp(m->commands[i].name, name) == 0)
      return &m->commands[i];
  }
  return NULL;
}

/* ---- lint --------------------------------------------------------------- */

/* Filesystem-flavoured names must be typed path, not string. */
static bool lint_pathish_name(const char *name) {
  static const char *exact[5] = {"path", "src", "dst", "file", "dir"};
  static const char *suffix[3] = {"_path", "_dir", "_file"};
  size_t i, nlen, slen;
  if (!name) return false;
  for (i = 0; i < 5; i++)
    if (strcmp(name, exact[i]) == 0) return true;
  nlen = strlen(name);
  for (i = 0; i < 3; i++) {
    slen = strlen(suffix[i]);
    if (nlen > slen && strcmp(name + nlen - slen, suffix[i]) == 0) return true;
  }
  return false;
}

/* Findings follow manifest order: the tool header first, then each command
 * with its rules in field order (description, annotations, timeout, params,
 * examples). Buffer append failures are swallowed (lint is best-effort)
 * but the error count stays exact. */
int astools_manifest_lint(const astools_manifest *m, astools_buf *out) {
  int errors = 0;
  size_t i, j;
  if (!m || !out) return 0;

  if (!m->description || astools_str_blank(m->description))
    (void)astools_buf_printf(out, "warn: tool '%s': no description\n",
                             m->id ? m->id : "?");

  for (i = 0; i < m->commands_len; i++) {
    const astools_cmd *c = &m->commands[i];
    const char *name = c->name ? c->name : "?";

    if (!c->description || astools_str_blank(c->description))
      (void)astools_buf_printf(out, "warn: command '%s': no description\n",
                               name);
    if (c->destructive && c->read_only) {
      (void)astools_buf_printf(
          out, "error: command '%s': destructive contradicts read_only\n",
          name);
      errors++;
    }
    if (c->long_running && c->timeout_ms == 0)
      (void)astools_buf_printf(
          out, "warn: command '%s': long_running but no timeout\n", name);
    for (j = 0; j < c->params_len; j++) {
      const astools_param *p = &c->params[j];
      if (p->type && p->type->kind == AT_STRING && lint_pathish_name(p->name)) {
        (void)astools_buf_printf(
            out,
            "error: command '%s': param '%s' must be typed path, not "
            "string\n",
            name, p->name);
        errors++;
      }
    }
    if (c->examples_len == 0)
      (void)astools_buf_printf(out, "warn: command '%s': no examples\n", name);
  }
  return errors;
}

/* ---- render ------------------------------------------------------------- */

static xcdn_node_t *wrap_value(xcdn_value_t *v) {
  xcdn_node_t *n;
  if (!v) return NULL;
  n = xcdn_node_new(v);
  if (!n) xcdn_value_free(v);
  return n;
}

/* xcdn_value_string does not report strdup failure; verify. */
static xcdn_value_t *val_string(const char *s) {
  xcdn_value_t *v;
  if (!s) return NULL;
  v = xcdn_value_string(s);
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

static xcdn_value_t *val_duration_ms(int64_t ms) {
  char buf[32];
  xcdn_value_t *v;
  snprintf(buf, sizeof(buf), "PT%lldS", (long long)(ms / 1000));
  v = xcdn_value_duration(buf);
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

/* Puts take ownership of the node/value and free it on failure. */
static bool obj_put(xcdn_value_t *obj, const char *key, xcdn_node_t *n) {
  if (!n) return false;
  if (!obj_append_checked(obj, key, n)) {
    xcdn_node_free(n);
    return false;
  }
  return true;
}

static bool obj_put_val(xcdn_value_t *obj, const char *key, xcdn_value_t *v) {
  return obj_put(obj, key, wrap_value(v));
}

static bool obj_put_str(xcdn_value_t *obj, const char *key, const char *s) {
  return obj_put_val(obj, key, val_string(s));
}

static bool obj_put_int(xcdn_value_t *obj, const char *key, int64_t v) {
  return obj_put_val(obj, key, xcdn_value_int(v));
}

static bool obj_put_bool(xcdn_value_t *obj, const char *key, bool v) {
  return obj_put_val(obj, key, xcdn_value_bool(v));
}

static bool arr_put(xcdn_value_t *arr, xcdn_node_t *n) {
  if (!n) return false;
  if (!arr_push_checked(arr, n)) {
    xcdn_node_free(n);
    return false;
  }
  return true;
}

/* Fresh object node carrying one tag; NULL on OOM. */
static xcdn_node_t *tagged_obj_node(const char *tag) {
  xcdn_node_t *n = wrap_value(xcdn_value_object());
  if (!n) return NULL;
  if (tag && !node_add_tag_checked(n, tag)) {
    xcdn_node_free(n);
    return NULL;
  }
  return n;
}

static xcdn_value_t *render_strv(char **v, size_t n) {
  xcdn_value_t *arr = xcdn_value_array();
  size_t i;
  if (!arr) return NULL;
  for (i = 0; i < n; i++) {
    if (!arr_put(arr, wrap_value(val_string(v[i])))) {
      xcdn_value_free(arr);
      return NULL;
    }
  }
  return arr;
}

static xcdn_node_t *render_param(const astools_param *p);

static const char *tkind_name(astools_tkind k) {
  switch (k) {
    case AT_STRING: return "string";
    case AT_INTEGER: return "integer";
    case AT_NUMBER: return "number";
    case AT_BOOLEAN: return "boolean";
    case AT_BYTES: return "bytes";
    case AT_DATETIME: return "datetime";
    case AT_DURATION: return "duration";
    case AT_UUID: return "uuid";
    case AT_PATH: return "path";
    case AT_ENUM: return "enum";
    case AT_ARRAY: return "array";
    case AT_MAP: return "map";
    case AT_OBJECT: return "object";
    default: return NULL;
  }
}

static xcdn_node_t *render_type(const astools_type *t) {
  xcdn_node_t *n;
  xcdn_value_t *o;
  const char *kind;
  size_t i;
  if (!t) return NULL;
  kind = tkind_name(t->kind);
  if (!kind) return NULL;
  n = tagged_obj_node("type");
  if (!n) return NULL;
  o = n->value;
  if (!obj_put_str(o, "kind", kind)) goto fail;
  switch (t->kind) {
    case AT_STRING:
    case AT_BYTES:
      if (t->min_len >= 0 && !obj_put_int(o, "min_len", t->min_len)) goto fail;
      if (t->max_len >= 0 && !obj_put_int(o, "max_len", t->max_len)) goto fail;
      break;
    case AT_INTEGER:
      if (t->has_min && !obj_put_int(o, "min", (int64_t)t->min_num)) goto fail;
      if (t->has_max && !obj_put_int(o, "max", (int64_t)t->max_num)) goto fail;
      break;
    case AT_NUMBER:
      if (t->has_min && !obj_put_val(o, "min", xcdn_value_float(t->min_num)))
        goto fail;
      if (t->has_max && !obj_put_val(o, "max", xcdn_value_float(t->max_num)))
        goto fail;
      break;
    case AT_PATH: {
      const char *acc = access_to_string(t->access);
      if (acc && !obj_put_str(o, "access", acc)) goto fail;
      if (t->must_exist && !obj_put_bool(o, "must_exist", true)) goto fail;
      break;
    }
    case AT_ENUM: {
      xcdn_value_t *vals = render_strv(t->values, t->values_len);
      if (!obj_put_val(o, "values", vals)) goto fail;
      break;
    }
    case AT_ARRAY:
      if (!obj_put(o, "item", render_type(t->item))) goto fail;
      if (t->min_items >= 0 && !obj_put_int(o, "min_items", t->min_items))
        goto fail;
      if (t->max_items >= 0 && !obj_put_int(o, "max_items", t->max_items))
        goto fail;
      break;
    case AT_MAP:
      if (!obj_put(o, "value", render_type(t->value))) goto fail;
      if (t->max_entries >= 0 && !obj_put_int(o, "max_entries", t->max_entries))
        goto fail;
      break;
    case AT_OBJECT: {
      xcdn_value_t *fields = xcdn_value_array();
      if (!fields) goto fail;
      for (i = 0; i < t->fields_len; i++) {
        if (!arr_put(fields, render_param(&t->fields[i]))) {
          xcdn_value_free(fields);
          goto fail;
        }
      }
      if (!obj_put_val(o, "fields", fields)) goto fail;
      break;
    }
    default:
      break; /* boolean/datetime/duration/uuid carry no attrs */
  }
  return n;
fail:
  xcdn_node_free(n);
  return NULL;
}

static xcdn_node_t *render_param(const astools_param *p) {
  xcdn_node_t *n;
  xcdn_value_t *o;
  if (!p || !p->name) return NULL;
  n = tagged_obj_node("param");
  if (!n) return NULL;
  o = n->value;
  if (!obj_put_str(o, "name", p->name)) goto fail;
  if (!obj_put(o, "type", render_type(p->type))) goto fail;
  if (p->required && !obj_put_bool(o, "required", true)) goto fail;
  if (p->dflt && !obj_put(o, "default", clone_node(p->dflt))) goto fail;
  if (p->description && !obj_put_str(o, "description", p->description))
    goto fail;
  return n;
fail:
  xcdn_node_free(n);
  return NULL;
}

static xcdn_node_t *render_runtime(const astools_manifest *m) {
  xcdn_node_t *n = tagged_obj_node(NULL);
  xcdn_value_t *o, *entries;
  size_t i;
  if (!n) return NULL;
  o = n->value;
  if (!obj_put_str(o, "mode", m->mode == ASTOOLS_MODE_PERSISTENT
                                  ? "persistent"
                                  : "oneshot"))
    goto fail;
  entries = xcdn_value_array();
  if (!entries) goto fail;
  for (i = 0; i < m->entries_len; i++) {
    const astools_entry *e = &m->entries[i];
    xcdn_node_t *en = tagged_obj_node(NULL);
    if (!en || !arr_push_checked(entries, en)) {
      xcdn_node_free(en);
      xcdn_value_free(entries);
      goto fail;
    }
    if (!obj_put_str(en->value, "os", e->os) ||
        !obj_put_str(en->value, "arch", e->arch) ||
        !obj_put_val(en->value, "argv", render_strv(e->argv, e->argv_len))) {
      xcdn_value_free(entries);
      goto fail;
    }
  }
  if (!obj_put_val(o, "entry", entries)) goto fail;
  if (!obj_put_int(o, "parallel", m->parallel)) goto fail;
  if (!obj_put_val(o, "idle_timeout", val_duration_ms(m->idle_timeout_ms)))
    goto fail;
  if (!obj_put_val(o, "startup_timeout",
                   val_duration_ms(m->startup_timeout_ms)))
    goto fail;
  return n;
fail:
  xcdn_node_free(n);
  return NULL;
}

static xcdn_node_t *render_permissions(const astools_manifest *m) {
  xcdn_node_t *n = tagged_obj_node(NULL);
  xcdn_value_t *o, *fs;
  size_t i;
  if (!n) return NULL;
  o = n->value;
  fs = xcdn_value_array();
  if (!fs) goto fail;
  for (i = 0; i < m->perms.fs_len; i++) {
    const char *acc = access_to_string(m->perms.fs[i].access);
    xcdn_node_t *fn = tagged_obj_node(NULL);
    if (!fn || !arr_push_checked(fs, fn)) {
      xcdn_node_free(fn);
      xcdn_value_free(fs);
      goto fail;
    }
    if (!acc || !obj_put_str(fn->value, "path", m->perms.fs[i].path) ||
        !obj_put_str(fn->value, "access", acc)) {
      xcdn_value_free(fs);
      goto fail;
    }
  }
  if (!obj_put_val(o, "fs", fs)) goto fail;
  if (!obj_put_bool(o, "net", m->perms.net)) goto fail;
  if (!obj_put_bool(o, "proc", m->perms.proc)) goto fail;
  if (!obj_put_val(o, "env", render_strv(m->perms.env, m->perms.env_len)))
    goto fail;
  return n;
fail:
  xcdn_node_free(n);
  return NULL;
}

static xcdn_node_t *render_command(const astools_cmd *c) {
  xcdn_node_t *n = tagged_obj_node("command");
  xcdn_value_t *o;
  size_t i;
  if (!n) return NULL;
  o = n->value;
  if (!obj_put_str(o, "name", c->name)) goto fail;
  if (!obj_put_str(o, "summary", c->summary)) goto fail;
  if (c->description && !obj_put_str(o, "description", c->description))
    goto fail;
  if (c->read_only || c->destructive || c->idempotent || c->long_running) {
    xcdn_node_t *an = tagged_obj_node(NULL);
    if (!an || !obj_append_checked(o, "annotations", an)) {
      xcdn_node_free(an);
      goto fail;
    }
    if (!obj_put_bool(an->value, "read_only", c->read_only) ||
        !obj_put_bool(an->value, "destructive", c->destructive) ||
        !obj_put_bool(an->value, "idempotent", c->idempotent) ||
        !obj_put_bool(an->value, "long_running", c->long_running))
      goto fail;
  }
  if (c->timeout_ms > 0 &&
      !obj_put_val(o, "timeout", val_duration_ms(c->timeout_ms)))
    goto fail;
  if (c->params_len > 0) {
    xcdn_value_t *params = xcdn_value_array();
    if (!params) goto fail;
    for (i = 0; i < c->params_len; i++) {
      if (!arr_put(params, render_param(&c->params[i]))) {
        xcdn_value_free(params);
        goto fail;
      }
    }
    if (!obj_put_val(o, "params", params)) goto fail;
  }
  if (c->returns_type) {
    xcdn_node_t *rn = tagged_obj_node(NULL);
    if (!rn || !obj_append_checked(o, "returns", rn)) {
      xcdn_node_free(rn);
      goto fail;
    }
    if (!obj_put(rn->value, "type", render_type(c->returns_type))) goto fail;
    if (c->returns_desc &&
        !obj_put_str(rn->value, "description", c->returns_desc))
      goto fail;
  }
  if (c->errors_len > 0) {
    xcdn_value_t *errs = xcdn_value_array();
    if (!errs) goto fail;
    for (i = 0; i < c->errors_len; i++) {
      xcdn_node_t *en = tagged_obj_node(NULL);
      if (!en || !arr_push_checked(errs, en)) {
        xcdn_node_free(en);
        xcdn_value_free(errs);
        goto fail;
      }
      if (!obj_put_str(en->value, "code", c->errors[i].code)) {
        xcdn_value_free(errs);
        goto fail;
      }
      if (c->errors[i].description &&
          !obj_put_str(en->value, "description", c->errors[i].description)) {
        xcdn_value_free(errs);
        goto fail;
      }
    }
    if (!obj_put_val(o, "errors", errs)) goto fail;
  }
  if (c->examples_len > 0) {
    xcdn_value_t *exs = xcdn_value_array();
    if (!exs) goto fail;
    for (i = 0; i < c->examples_len; i++) {
      const astools_example *ex = &c->examples[i];
      xcdn_node_t *en = tagged_obj_node("example");
      if (!en || !arr_push_checked(exs, en)) {
        xcdn_node_free(en);
        xcdn_value_free(exs);
        goto fail;
      }
      if (ex->title && !obj_put_str(en->value, "title", ex->title)) {
        xcdn_value_free(exs);
        goto fail;
      }
      if (!obj_put(en->value, "call", clone_node(ex->call))) {
        xcdn_value_free(exs);
        goto fail;
      }
      if (ex->result && !obj_put(en->value, "result", clone_node(ex->result))) {
        xcdn_value_free(exs);
        goto fail;
      }
      if (ex->note && !obj_put_str(en->value, "note", ex->note)) {
        xcdn_value_free(exs);
        goto fail;
      }
    }
    if (!obj_put_val(o, "examples", exs)) goto fail;
  }
  if (c->deprecated && !obj_put_bool(o, "deprecated", true)) goto fail;
  return n;
fail:
  xcdn_node_free(n);
  return NULL;
}

/* Canonical re-serialization: fields in order, defaults materialized,
 * absent optionals omitted, durations as r"PT<seconds>S". */
astools_err astools_manifest_render(const astools_manifest *m, char **out) {
  xcdn_document_t *doc = NULL;
  xcdn_node_t *root = NULL;
  xcdn_value_t *o, *cmds;
  char *text;
  size_t i;

  if (!m || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  doc = xcdn_document_new();
  if (!doc) return ASTOOLS_ERR_NOMEM;
  root = tagged_obj_node("astools_tool");
  if (!root) goto fail;
  o = root->value;

  if (!obj_put_int(o, "manifest_version", m->manifest_version)) goto fail;
  if (!obj_put_str(o, "id", m->id)) goto fail;
  if (!obj_put_str(o, "version", m->version)) goto fail;
  if (!obj_put_str(o, "title", m->title)) goto fail;
  if (!obj_put_str(o, "summary", m->summary)) goto fail;
  if (m->description && !obj_put_str(o, "description", m->description))
    goto fail;
  if (!obj_put_str(o, "kind", m->kind == ASTOOLS_KIND_LIBRARY ? "library"
                                                              : "executable"))
    goto fail;
  if (!obj_put_val(o, "platforms",
                   render_strv(m->platforms, m->platforms_len)))
    goto fail;
  if (!obj_put(o, "runtime", render_runtime(m))) goto fail;
  if (!obj_put(o, "permissions", render_permissions(m))) goto fail;

  cmds = xcdn_value_array();
  if (!cmds) goto fail;
  for (i = 0; i < m->commands_len; i++) {
    if (!arr_put(cmds, render_command(&m->commands[i]))) {
      xcdn_value_free(cmds);
      goto fail;
    }
  }
  if (!obj_put_val(o, "commands", cmds)) goto fail;

  if (!doc_push_checked(doc, root)) goto fail;
  root = NULL; /* owned by doc now */

  text = xcdn_to_string_pretty(doc);
  xcdn_document_free(doc);
  if (!text) return ASTOOLS_ERR_NOMEM;
  *out = text;
  return ASTOOLS_OK;

fail:
  xcdn_node_free(root);
  xcdn_document_free(doc);
  return ASTOOLS_ERR_NOMEM;
}
