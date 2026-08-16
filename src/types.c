/*
 * types.c — the type system: #type parsing, strict recursive value
 * validation (no coercion), and default injection.
 *
 * Everything parsed or checked here is hostile input (manifests, args).
 * Invariants kept throughout:
 *  - recursion depth is capped (TYPE_DEPTH_MAX for types, CLONE_DEPTH_MAX
 *    for cloned literals) so no input can overflow the stack;
 *  - partially-built xcdn clones always keep len fields consistent so
 *    xcdn_node_free/xcdn_value_free can release them at any point;
 *  - object growth for default injection is done locally because the xcdn
 *    mutators do not report allocation failure.
 */

#include "astools_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define TYPE_DEPTH_MAX 32
#define CLONE_DEPTH_MAX 64

/* 2^63 as a double; exact, used to guard int64 <-> double comparisons. */
#define TWO_POW_63 9223372036854775808.0

static astools_type *type_parse_at(const xcdn_node_t *node, const char *ctx,
                                   int depth, char *err, size_t err_cap);
static bool param_parse(const xcdn_node_t *pn, const char *ctx, int depth,
                        astools_param *out, char *err, size_t err_cap);
static xcdn_node_t *clone_node(const xcdn_node_t *n, int depth);
static xcdn_value_t *clone_value(const xcdn_value_t *v, int depth);

/* ---- error formatting ---------------------------------------------------- */

static void tset_err(char *err, size_t err_cap, const char *fmt, ...) {
  va_list ap;
  if (!err || err_cap == 0) return;
  va_start(ap, fmt);
  vsnprintf(err, err_cap, fmt, ap);
  va_end(ap);
}

/* Human name of an xcdn value for "expected X, got Y" messages. */
static const char *vt_name(const xcdn_value_t *v) {
  if (!v) return "null";
  switch (v->type) {
    case XCDN_VAL_NULL: return "null";
    case XCDN_VAL_BOOL: return "boolean";
    case XCDN_VAL_INT: return "integer";
    case XCDN_VAL_FLOAT: return "number";
    case XCDN_VAL_DECIMAL: return "decimal";
    case XCDN_VAL_STRING: return "string";
    case XCDN_VAL_BYTES: return "bytes";
    case XCDN_VAL_DATETIME: return "datetime";
    case XCDN_VAL_DURATION: return "duration";
    case XCDN_VAL_UUID: return "uuid";
    case XCDN_VAL_ARRAY: return "array";
    case XCDN_VAL_OBJECT: return "object";
    default: return "unknown";
  }
}

static astools_err mismatch(const char *ctx, const char *expect,
                            const xcdn_value_t *got, char *err,
                            size_t err_cap) {
  tset_err(err, err_cap, "%s: expected %s, got %s", ctx, expect,
           vt_name(got));
  return ASTOOLS_ERR_INVALID;
}

/* ---- kind tables --------------------------------------------------------- */

static bool kind_lookup(const char *s, astools_tkind *out) {
  static const struct {
    const char *name;
    astools_tkind kind;
  } tab[] = {
      {"string", AT_STRING},     {"integer", AT_INTEGER},
      {"number", AT_NUMBER},     {"boolean", AT_BOOLEAN},
      {"bytes", AT_BYTES},       {"datetime", AT_DATETIME},
      {"duration", AT_DURATION}, {"uuid", AT_UUID},
      {"path", AT_PATH},         {"enum", AT_ENUM},
      {"array", AT_ARRAY},       {"map", AT_MAP},
      {"object", AT_OBJECT}};
  size_t i;
  for (i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
    if (strcmp(tab[i].name, s) == 0) {
      *out = tab[i].kind;
      return true;
    }
  }
  return false;
}

static const char *kind_name(astools_tkind k) {
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
    default: return "unknown";
  }
}

/* Constraint keys accepted for a kind ("kind" always). names has room >=4. */
static void kind_keys(astools_tkind k, const char **names, size_t *n) {
  size_t i = 0;
  names[i++] = "kind";
  switch (k) {
    case AT_STRING:
    case AT_BYTES:
      names[i++] = "min_len";
      names[i++] = "max_len";
      break;
    case AT_INTEGER:
    case AT_NUMBER:
      names[i++] = "min";
      names[i++] = "max";
      break;
    case AT_PATH:
      names[i++] = "access";
      names[i++] = "must_exist";
      break;
    case AT_ENUM:
      names[i++] = "values";
      break;
    case AT_ARRAY:
      names[i++] = "item";
      names[i++] = "min_items";
      names[i++] = "max_items";
      break;
    case AT_MAP:
      names[i++] = "value";
      names[i++] = "max_entries";
      break;
    case AT_OBJECT:
      names[i++] = "fields";
      break;
    default: /* boolean/datetime/duration/uuid: bare */
      break;
  }
  *n = i;
}

/* ---- duplicate-key detection --------------------------------------------- */

static const char *dup_str_pairwise(const char *const *names, size_t n) {
  size_t i, j;
  for (i = 0; i + 1 < n; i++) {
    if (!names[i]) continue;
    for (j = i + 1; j < n; j++)
      if (names[j] && strcmp(names[i], names[j]) == 0) return names[i];
  }
  return NULL;
}

/* First duplicated string, or NULL. Linear-time hashed probe for large n
 * (hostile inputs may carry huge objects); falls back to pairwise when the
 * table cannot be allocated. */
static const char *dup_str(const char *const *names, size_t n) {
  size_t cap = 1, i, *slots;
  const char *found = NULL;
  if (n < 2) return NULL;
  if (n <= 64 || n > (SIZE_MAX >> 3)) return dup_str_pairwise(names, n);
  while (cap < n * 2) cap <<= 1;
  slots = calloc(cap, sizeof(size_t)); /* index+1; 0 = empty */
  if (!slots) return dup_str_pairwise(names, n);
  for (i = 0; i < n && !found; i++) {
    size_t h;
    if (!names[i]) continue;
    h = (size_t)astools_fnv1a64(names[i], strlen(names[i])) & (cap - 1);
    for (;;) {
      if (slots[h] == 0) {
        slots[h] = i + 1;
        break;
      }
      if (strcmp(names[slots[h] - 1], names[i]) == 0) {
        found = names[i];
        break;
      }
      h = (h + 1) & (cap - 1);
    }
  }
  free(slots);
  return found;
}

/* Reject duplicate keys in an OBJECT value (xcdn preserves them; a tool
 * must never see a key twice while validation saw only the first). */
static astools_err check_obj_dups(const xcdn_value_t *obj, const char *ctx,
                                  char *err, size_t err_cap) {
  const char **keys;
  const char *dup;
  size_t n, i;
  n = obj->data.object.len;
  if (n < 2) return ASTOOLS_OK;
  if (n > SIZE_MAX / sizeof(*keys)) {
    tset_err(err, err_cap, "%s: out of memory", ctx);
    return ASTOOLS_ERR_NOMEM;
  }
  keys = malloc(n * sizeof(*keys));
  if (!keys) {
    tset_err(err, err_cap, "%s: out of memory", ctx);
    return ASTOOLS_ERR_NOMEM;
  }
  for (i = 0; i < n; i++) keys[i] = obj->data.object.entries[i].key;
  dup = dup_str(keys, n);
  free(keys);
  if (dup) {
    tset_err(err, err_cap, "%s: duplicate key \"%.64s\"", ctx, dup);
    return ASTOOLS_ERR_INVALID;
  }
  return ASTOOLS_OK;
}

/* ---- int64 vs double bound comparison ------------------------------------ */

/* Bounds are stored as doubles (contract); comparing (double)i directly is
 * lossy above 2^53, so compare against ceil/floor of the bound instead. */
static bool int_ge(int64_t i, double bound) {
  double c;
  if (bound >= TWO_POW_63) return false;
  if (bound < -TWO_POW_63) return true;
  c = ceil(bound);
  if (c >= TWO_POW_63) return false;
  return i >= (int64_t)c; /* c integral in [-2^63, 2^63): representable */
}

static bool int_le(int64_t i, double bound) {
  double f;
  if (bound >= TWO_POW_63) return true;
  if (bound < -TWO_POW_63) return false;
  f = floor(bound);
  return i <= (int64_t)f; /* f integral in [-2^63, 2^63): representable */
}

/* ---- deep clone of xcdn trees -------------------------------------------- */

/*
 * xcdn has no public clone and its mutators do not report OOM, so the
 * copies are built by hand on the public structs. len fields are advanced
 * only over fully-initialized elements so a partial clone is always
 * releasable with the xcdn destructors.
 */
static xcdn_value_t *clone_value(const xcdn_value_t *v, int depth) {
  xcdn_value_t *out;
  if (!v || depth > CLONE_DEPTH_MAX) return NULL;
  out = calloc(1, sizeof(*out));
  if (!out) return NULL;
  out->type = v->type;
  switch (v->type) {
    case XCDN_VAL_NULL:
      break;
    case XCDN_VAL_BOOL:
      out->data.boolean = v->data.boolean;
      break;
    case XCDN_VAL_INT:
      out->data.integer = v->data.integer;
      break;
    case XCDN_VAL_FLOAT:
      out->data.floating = v->data.floating;
      break;
    case XCDN_VAL_DECIMAL:
    case XCDN_VAL_STRING:
    case XCDN_VAL_DATETIME:
    case XCDN_VAL_DURATION:
    case XCDN_VAL_UUID:
      if (v->data.string) {
        out->data.string = astools_strdup(v->data.string);
        if (!out->data.string) goto fail;
      }
      break;
    case XCDN_VAL_BYTES:
      if (v->data.bytes.data && v->data.bytes.len > 0) {
        out->data.bytes.data = malloc(v->data.bytes.len);
        if (!out->data.bytes.data) goto fail;
        memcpy(out->data.bytes.data, v->data.bytes.data, v->data.bytes.len);
        out->data.bytes.len = v->data.bytes.len;
      }
      break;
    case XCDN_VAL_ARRAY: {
      size_t i, n = v->data.array.len;
      if (n > 0) {
        if (n > SIZE_MAX / sizeof(xcdn_node_t *)) goto fail;
        out->data.array.items = calloc(n, sizeof(xcdn_node_t *));
        if (!out->data.array.items) goto fail;
        out->data.array.cap = n;
        for (i = 0; i < n; i++) {
          xcdn_node_t *cn = clone_node(v->data.array.items[i], depth + 1);
          if (!cn) goto fail;
          out->data.array.items[i] = cn;
          out->data.array.len = i + 1;
        }
      }
      break;
    }
    case XCDN_VAL_OBJECT: {
      size_t i, n = v->data.object.len;
      if (n > 0) {
        if (n > SIZE_MAX / sizeof(xcdn_object_entry_t)) goto fail;
        out->data.object.entries = calloc(n, sizeof(xcdn_object_entry_t));
        if (!out->data.object.entries) goto fail;
        out->data.object.cap = n;
        for (i = 0; i < n; i++) {
          const xcdn_object_entry_t *se = &v->data.object.entries[i];
          xcdn_object_entry_t *de = &out->data.object.entries[i];
          de->key = astools_strdup(se->key ? se->key : "");
          if (!de->key) goto fail;
          de->node = clone_node(se->node, depth + 1);
          if (!de->node) {
            free(de->key);
            de->key = NULL;
            goto fail;
          }
          out->data.object.len = i + 1;
        }
      }
      break;
    }
    default:
      goto fail; /* unknown value type: refuse to copy */
  }
  return out;
fail:
  xcdn_value_free(out);
  return NULL;
}

static xcdn_node_t *clone_node(const xcdn_node_t *n, int depth) {
  xcdn_node_t *out;
  size_t i;
  if (!n || depth > CLONE_DEPTH_MAX) return NULL;
  out = calloc(1, sizeof(*out));
  if (!out) return NULL;
  if (n->tags_len > 0) {
    if (n->tags_len > SIZE_MAX / sizeof(xcdn_tag_t)) goto fail;
    out->tags = calloc(n->tags_len, sizeof(xcdn_tag_t));
    if (!out->tags) goto fail;
    out->tags_cap = n->tags_len;
    for (i = 0; i < n->tags_len; i++) {
      out->tags[i].name =
          astools_strdup(n->tags[i].name ? n->tags[i].name : "");
      if (!out->tags[i].name) goto fail;
      out->tags_len = i + 1;
    }
  }
  if (n->annotations_len > 0) {
    if (n->annotations_len > SIZE_MAX / sizeof(xcdn_annotation_t)) goto fail;
    out->annotations = calloc(n->annotations_len, sizeof(xcdn_annotation_t));
    if (!out->annotations) goto fail;
    out->annotations_cap = n->annotations_len;
    for (i = 0; i < n->annotations_len; i++) {
      const xcdn_annotation_t *sa = &n->annotations[i];
      xcdn_annotation_t *da = &out->annotations[i];
      size_t j;
      da->name = astools_strdup(sa->name ? sa->name : "");
      if (!da->name) goto fail;
      out->annotations_len = i + 1; /* name owned from here on */
      if (sa->args_len > 0) {
        if (sa->args_len > SIZE_MAX / sizeof(xcdn_value_t *)) goto fail;
        da->args = calloc(sa->args_len, sizeof(xcdn_value_t *));
        if (!da->args) goto fail;
        da->args_cap = sa->args_len;
        for (j = 0; j < sa->args_len; j++) {
          da->args[j] = clone_value(sa->args[j], depth + 1);
          if (!da->args[j]) goto fail;
          da->args_len = j + 1;
        }
      }
    }
  }
  if (n->value) {
    out->value = clone_value(n->value, depth + 1);
    if (!out->value) goto fail;
  }
  return out;
fail:
  xcdn_node_free(out);
  return NULL;
}

/* ---- default injection --------------------------------------------------- */

/* Insert a deep copy of dflt under key (caller ensured key is absent).
 * Growth is done here, checked, instead of via xcdn_object_set, whose
 * allocation failures are silent. */
static astools_err obj_inject_default(xcdn_value_t *obj, const char *key,
                                      const xcdn_node_t *dflt) {
  xcdn_node_t *copy;
  xcdn_object_entry_t *e;
  if (!obj || obj->type != XCDN_VAL_OBJECT || !key)
    return ASTOOLS_ERR_INVALID;
  copy = clone_node(dflt, 0);
  if (!copy) return ASTOOLS_ERR_NOMEM;
  if (obj->data.object.len >= obj->data.object.cap) {
    size_t ncap = obj->data.object.cap ? obj->data.object.cap * 2 : 4;
    xcdn_object_entry_t *ne;
    if (ncap <= obj->data.object.len ||
        ncap > SIZE_MAX / sizeof(*ne)) {
      xcdn_node_free(copy);
      return ASTOOLS_ERR_NOMEM;
    }
    ne = realloc(obj->data.object.entries, ncap * sizeof(*ne));
    if (!ne) {
      xcdn_node_free(copy);
      return ASTOOLS_ERR_NOMEM;
    }
    obj->data.object.entries = ne;
    obj->data.object.cap = ncap;
  }
  e = &obj->data.object.entries[obj->data.object.len];
  e->key = astools_strdup(key);
  if (!e->key) {
    xcdn_node_free(copy);
    return ASTOOLS_ERR_NOMEM;
  }
  e->node = copy;
  obj->data.object.len++;
  return ASTOOLS_OK;
}

/* ---- constraint field readers -------------------------------------------- */

/* Optional non-negative integer field; leaves *out untouched when absent. */
static bool opt_nonneg(const xcdn_value_t *ov, const char *ctx,
                       const char *field, int64_t *out, char *err,
                       size_t err_cap) {
  const xcdn_node_t *n = xcdn_object_get(ov, field);
  if (!n) return true;
  if (!n->value || n->value->type != XCDN_VAL_INT) {
    tset_err(err, err_cap, "%s: \"%s\": expected integer, got %s", ctx,
             field, vt_name(n->value));
    return false;
  }
  if (n->value->data.integer < 0) {
    tset_err(err, err_cap, "%s: \"%s\" must not be negative", ctx, field);
    return false;
  }
  *out = n->value->data.integer;
  return true;
}

/* Optional numeric bound (integer or float literal, finite). */
static bool opt_bound(const xcdn_value_t *ov, const char *ctx,
                      const char *field, bool *has, double *out, char *err,
                      size_t err_cap) {
  const xcdn_node_t *n = xcdn_object_get(ov, field);
  double d;
  if (!n) return true;
  if (n->value && n->value->type == XCDN_VAL_INT) {
    d = (double)n->value->data.integer;
  } else if (n->value && n->value->type == XCDN_VAL_FLOAT) {
    d = n->value->data.floating;
  } else {
    tset_err(err, err_cap, "%s: \"%s\": expected number, got %s", ctx,
             field, vt_name(n->value));
    return false;
  }
  if (!isfinite(d)) {
    tset_err(err, err_cap, "%s: \"%s\" must be finite", ctx, field);
    return false;
  }
  *has = true;
  *out = d;
  return true;
}

/* Optional boolean field. */
static bool opt_bool(const xcdn_value_t *ov, const char *ctx,
                     const char *field, bool *out, char *err,
                     size_t err_cap) {
  const xcdn_node_t *n = xcdn_object_get(ov, field);
  if (!n) return true;
  if (!n->value || n->value->type != XCDN_VAL_BOOL) {
    tset_err(err, err_cap, "%s: \"%s\": expected boolean, got %s", ctx,
             field, vt_name(n->value));
    return false;
  }
  *out = n->value->data.boolean;
  return true;
}

/* ---- #type parsing ------------------------------------------------------- */

static astools_type *type_new(astools_tkind k) {
  astools_type *t = calloc(1, sizeof(*t));
  if (!t) return NULL;
  t->kind = k;
  t->min_len = -1;
  t->max_len = -1;
  t->min_items = -1;
  t->max_items = -1;
  t->max_entries = -1;
  t->access = ASTOOLS_ACCESS_READ;
  return t;
}

/* Parameter names: lowercase C-identifier style, at most 64 chars. Not a
 * slug (is for tool/command names); underscores are the convention. */
static bool param_name_ok(const char *s) {
  size_t i;
  if (!s || s[0] == '\0') return false;
  if (!((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
  for (i = 0; s[i] != '\0'; i++) {
    char ch = s[i];
    if (i >= 64) return false;
    if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
          ch == '_'))
      return false;
  }
  return true;
}

static astools_type *type_parse_at(const xcdn_node_t *node, const char *ctx,
                                   int depth, char *err, size_t err_cap) {
  const xcdn_value_t *ov;
  const xcdn_node_t *kn;
  const char *keys[4];
  size_t keys_n, i;
  astools_tkind kind;
  astools_type *t;

  if (depth > TYPE_DEPTH_MAX) {
    tset_err(err, err_cap, "%s: type nesting exceeds %d levels", ctx,
             TYPE_DEPTH_MAX);
    return NULL;
  }
  if (!node || !node->value || node->value->type != XCDN_VAL_OBJECT) {
    tset_err(err, err_cap, "%s: expected type object, got %s", ctx,
             vt_name(node ? node->value : NULL));
    return NULL;
  }
  ov = node->value;
  if (check_obj_dups(ov, ctx, err, err_cap) != ASTOOLS_OK) return NULL;

  kn = xcdn_object_get(ov, "kind");
  if (!kn || !kn->value || kn->value->type != XCDN_VAL_STRING ||
      !kn->value->data.string) {
    tset_err(err, err_cap, "%s: missing string field \"kind\"", ctx);
    return NULL;
  }
  if (!kind_lookup(kn->value->data.string, &kind)) {
    tset_err(err, err_cap, "%s: unknown kind \"%.32s\"", ctx,
             kn->value->data.string);
    return NULL;
  }

  kind_keys(kind, keys, &keys_n);
  for (i = 0; i < ov->data.object.len; i++) {
    const char *k = ov->data.object.entries[i].key;
    size_t j;
    bool known = false;
    for (j = 0; j < keys_n; j++)
      if (k && strcmp(k, keys[j]) == 0) {
        known = true;
        break;
      }
    if (!known) {
      tset_err(err, err_cap, "%s: unknown field \"%.64s\" for kind \"%s\"",
               ctx, k ? k : "(null)", kind_name(kind));
      return NULL;
    }
  }

  t = type_new(kind);
  if (!t) {
    tset_err(err, err_cap, "%s: out of memory", ctx);
    return NULL;
  }

  switch (kind) {
    case AT_STRING:
    case AT_BYTES:
      if (!opt_nonneg(ov, ctx, "min_len", &t->min_len, err, err_cap))
        goto fail;
      if (!opt_nonneg(ov, ctx, "max_len", &t->max_len, err, err_cap))
        goto fail;
      if (t->min_len >= 0 && t->max_len >= 0 && t->max_len < t->min_len) {
        tset_err(err, err_cap, "%s: max_len is below min_len", ctx);
        goto fail;
      }
      break;
    case AT_INTEGER:
    case AT_NUMBER:
      if (!opt_bound(ov, ctx, "min", &t->has_min, &t->min_num, err,
                     err_cap))
        goto fail;
      if (!opt_bound(ov, ctx, "max", &t->has_max, &t->max_num, err,
                     err_cap))
        goto fail;
      if (t->has_min && t->has_max && t->min_num > t->max_num) {
        tset_err(err, err_cap, "%s: max is below min", ctx);
        goto fail;
      }
      break;
    case AT_BOOLEAN:
    case AT_DATETIME:
    case AT_DURATION:
    case AT_UUID:
      break;
    case AT_PATH: {
      const xcdn_node_t *an = xcdn_object_get(ov, "access");
      if (an) {
        const char *s =
            (an->value && an->value->type == XCDN_VAL_STRING)
                ? an->value->data.string
                : NULL;
        if (s && strcmp(s, "read") == 0) {
          t->access = ASTOOLS_ACCESS_READ;
        } else if (s && strcmp(s, "write") == 0) {
          t->access = ASTOOLS_ACCESS_WRITE;
        } else if (s && strcmp(s, "read-write") == 0) {
          t->access = ASTOOLS_ACCESS_RW;
        } else {
          tset_err(err, err_cap,
                   "%s: \"access\" must be \"read\", \"write\" or "
                   "\"read-write\"",
                   ctx);
          goto fail;
        }
      }
      if (!opt_bool(ov, ctx, "must_exist", &t->must_exist, err, err_cap))
        goto fail;
      break;
    }
    case AT_ENUM: {
      const xcdn_node_t *vn = xcdn_object_get(ov, "values");
      size_t n, k;
      if (!vn || !vn->value || vn->value->type != XCDN_VAL_ARRAY) {
        tset_err(err, err_cap,
                 "%s: enum requires \"values\" as an array of strings",
                 ctx);
        goto fail;
      }
      n = vn->value->data.array.len;
      if (n == 0) {
        tset_err(err, err_cap, "%s: enum \"values\" must not be empty",
                 ctx);
        goto fail;
      }
      if (n > SIZE_MAX / sizeof(char *)) {
        tset_err(err, err_cap, "%s: out of memory", ctx);
        goto fail;
      }
      t->values = calloc(n, sizeof(char *));
      if (!t->values) {
        tset_err(err, err_cap, "%s: out of memory", ctx);
        goto fail;
      }
      for (k = 0; k < n; k++) {
        const xcdn_node_t *en = vn->value->data.array.items[k];
        const char *s =
            (en && en->value && en->value->type == XCDN_VAL_STRING)
                ? en->value->data.string
                : NULL;
        if (!s || s[0] == '\0') {
          tset_err(err, err_cap,
                   "%s: enum values[%zu] must be a non-empty string", ctx,
                   k);
          goto fail;
        }
        t->values[k] = astools_strdup(s);
        if (!t->values[k]) {
          tset_err(err, err_cap, "%s: out of memory", ctx);
          goto fail;
        }
        t->values_len = k + 1;
      }
      break;
    }
    case AT_ARRAY: {
      const xcdn_node_t *in = xcdn_object_get(ov, "item");
      char cbuf[192];
      if (!in) {
        tset_err(err, err_cap, "%s: array requires \"item\"", ctx);
        goto fail;
      }
      snprintf(cbuf, sizeof cbuf, "%.185s.item", ctx);
      t->item = type_parse_at(in, cbuf, depth + 1, err, err_cap);
      if (!t->item) goto fail; /* err already set */
      if (!opt_nonneg(ov, ctx, "min_items", &t->min_items, err, err_cap))
        goto fail;
      if (!opt_nonneg(ov, ctx, "max_items", &t->max_items, err, err_cap))
        goto fail;
      if (t->min_items >= 0 && t->max_items >= 0 &&
          t->max_items < t->min_items) {
        tset_err(err, err_cap, "%s: max_items is below min_items", ctx);
        goto fail;
      }
      break;
    }
    case AT_MAP: {
      const xcdn_node_t *vn = xcdn_object_get(ov, "value");
      char cbuf[192];
      if (!vn) {
        tset_err(err, err_cap, "%s: map requires \"value\"", ctx);
        goto fail;
      }
      snprintf(cbuf, sizeof cbuf, "%.185s.value", ctx);
      t->value = type_parse_at(vn, cbuf, depth + 1, err, err_cap);
      if (!t->value) goto fail;
      if (!opt_nonneg(ov, ctx, "max_entries", &t->max_entries, err,
                      err_cap))
        goto fail;
      break;
    }
    case AT_OBJECT: {
      const xcdn_node_t *fn = xcdn_object_get(ov, "fields");
      size_t n, k;
      if (!fn || !fn->value || fn->value->type != XCDN_VAL_ARRAY) {
        tset_err(err, err_cap,
                 "%s: object requires \"fields\" as an array of params",
                 ctx);
        goto fail;
      }
      n = fn->value->data.array.len;
      if (n > 0) {
        const char **fnames;
        const char *dup;
        if (n > SIZE_MAX / sizeof(astools_param)) {
          tset_err(err, err_cap, "%s: out of memory", ctx);
          goto fail;
        }
        t->fields = calloc(n, sizeof(astools_param));
        if (!t->fields) {
          tset_err(err, err_cap, "%s: out of memory", ctx);
          goto fail;
        }
        for (k = 0; k < n; k++) {
          char cbuf[192];
          snprintf(cbuf, sizeof cbuf, "%.161s.fields[%zu]", ctx, k);
          if (!param_parse(fn->value->data.array.items[k], cbuf, depth,
                           &t->fields[k], err, err_cap))
            goto fail;
          t->fields_len = k + 1;
        }
        fnames = malloc(n * sizeof(*fnames));
        if (!fnames) {
          tset_err(err, err_cap, "%s: out of memory", ctx);
          goto fail;
        }
        for (k = 0; k < n; k++) fnames[k] = t->fields[k].name;
        dup = dup_str(fnames, n);
        free(fnames);
        if (dup) {
          tset_err(err, err_cap, "%s: duplicate field \"%.64s\"", ctx,
                   dup);
          goto fail;
        }
      }
      break;
    }
    default:
      tset_err(err, err_cap, "%s: internal: bad kind", ctx);
      goto fail;
  }
  return t;
fail:
  astools_type_free(t);
  return NULL;
}

/* #param parsing for object "fields". manifest.c parses top-level command
 * params itself and calls astools_type_parse per param type, so nothing
 * extra is exported from here. */
static bool param_parse(const xcdn_node_t *pn, const char *ctx, int depth,
                        astools_param *out, char *err, size_t err_cap) {
  static const char *const known[] = {"name",    "type",        "required",
                                      "default", "description", "examples"};
  const xcdn_value_t *ov;
  const xcdn_node_t *n;
  char cbuf[192];
  size_t i, j;

  if (!pn || !pn->value || pn->value->type != XCDN_VAL_OBJECT) {
    tset_err(err, err_cap, "%s: expected param object, got %s", ctx,
             vt_name(pn ? pn->value : NULL));
    return false;
  }
  ov = pn->value;
  if (check_obj_dups(ov, ctx, err, err_cap) != ASTOOLS_OK) return false;
  for (i = 0; i < ov->data.object.len; i++) {
    const char *k = ov->data.object.entries[i].key;
    bool ok = false;
    for (j = 0; j < sizeof(known) / sizeof(known[0]); j++)
      if (k && strcmp(k, known[j]) == 0) {
        ok = true;
        break;
      }
    if (!ok) {
      tset_err(err, err_cap, "%s: unknown field \"%.64s\"", ctx,
               k ? k : "(null)");
      return false;
    }
  }

  n = xcdn_object_get(ov, "name");
  {
    const char *s = (n && n->value && n->value->type == XCDN_VAL_STRING)
                        ? n->value->data.string
                        : NULL;
    if (!s) {
      tset_err(err, err_cap, "%s: missing string field \"name\"", ctx);
      return false;
    }
    if (!param_name_ok(s)) {
      tset_err(err, err_cap, "%s: invalid param name \"%.64s\"", ctx, s);
      return false;
    }
    out->name = astools_strdup(s);
    if (!out->name) {
      tset_err(err, err_cap, "%s: out of memory", ctx);
      return false;
    }
  }

  if (!opt_bool(ov, ctx, "required", &out->required, err, err_cap))
    goto fail;

  n = xcdn_object_get(ov, "description");
  if (n) {
    if (!n->value || n->value->type != XCDN_VAL_STRING ||
        !n->value->data.string) {
      tset_err(err, err_cap, "%s: \"description\": expected string, got %s",
               ctx, vt_name(n->value));
      goto fail;
    }
    out->description = astools_strdup(n->value->data.string);
    if (!out->description) {
      tset_err(err, err_cap, "%s: out of memory", ctx);
      goto fail;
    }
  }

  /* examples are advisory and kept only in the manifest text. */
  n = xcdn_object_get(ov, "examples");
  if (n && (!n->value || n->value->type != XCDN_VAL_ARRAY)) {
    tset_err(err, err_cap, "%s: \"examples\": expected array, got %s", ctx,
             vt_name(n->value));
    goto fail;
  }

  n = xcdn_object_get(ov, "type");
  if (!n) {
    tset_err(err, err_cap, "%s: missing field \"type\"", ctx);
    goto fail;
  }
  snprintf(cbuf, sizeof cbuf, "%.186s.type", ctx);
  out->type = type_parse_at(n, cbuf, depth + 1, err, err_cap);
  if (!out->type) goto fail;

  n = xcdn_object_get(ov, "default");
  if (n) {
    if (out->required) {
      tset_err(err, err_cap,
               "%s: \"default\" is only allowed on optional params", ctx);
      goto fail;
    }
    out->dflt = clone_node(n, 0);
    if (!out->dflt) {
      tset_err(err, err_cap,
               "%s: cannot copy default (too deep or out of memory)", ctx);
      goto fail;
    }
    /* Validate the stored default; the check also injects any nested
     * defaults into our owned copy, canonicalizing it once. */
    snprintf(cbuf, sizeof cbuf, "%.183s.default", ctx);
    if (astools_type_check(out->type, out->dflt, cbuf, err, err_cap) !=
        ASTOOLS_OK)
      goto fail;
  }
  return true;
fail:
  astools_param_free_fields(out);
  return false;
}

astools_type *astools_type_parse(const xcdn_node_t *node, char *err,
                                 size_t err_cap) {
  if (err && err_cap > 0) err[0] = '\0';
  return type_parse_at(node, "type", 0, err, err_cap);
}

void astools_type_free(astools_type *t) {
  size_t i;
  if (!t) return;
  for (i = 0; i < t->values_len; i++) free(t->values[i]);
  free(t->values);
  astools_type_free(t->item);
  astools_type_free(t->value);
  for (i = 0; i < t->fields_len; i++)
    astools_param_free_fields(&t->fields[i]);
  free(t->fields);
  free(t);
}

void astools_param_free_fields(astools_param *p) {
  if (!p) return;
  free(p->name);
  astools_type_free(p->type);
  xcdn_node_free(p->dflt);
  free(p->description);
  p->name = NULL;
  p->type = NULL;
  p->dflt = NULL;
  p->description = NULL;
  p->required = false;
}

/* ---- strict validation --------------------------------------------------- */

astools_err astools_type_check(const astools_type *t, const xcdn_node_t *v,
                               const char *ctxname, char *err,
                               size_t err_cap) {
  const char *ctx = ctxname ? ctxname : "value";
  const xcdn_value_t *val;
  astools_err e;

  if (!t) {
    tset_err(err, err_cap, "%s: internal: missing type", ctx);
    return ASTOOLS_ERR_INVALID;
  }
  if (!v || !v->value) {
    tset_err(err, err_cap, "%s: missing value", ctx);
    return ASTOOLS_ERR_INVALID;
  }
  val = v->value;

  switch (t->kind) {
    case AT_STRING: {
      size_t len;
      if (val->type != XCDN_VAL_STRING)
        return mismatch(ctx, "string", val, err, err_cap);
      if (!val->data.string) {
        tset_err(err, err_cap, "%s: invalid string value", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      len = strlen(val->data.string);
      if (t->min_len >= 0 && (int64_t)len < t->min_len) {
        tset_err(err, err_cap,
                 "%s: string length %zu is below min_len %lld", ctx, len,
                 (long long)t->min_len);
        return ASTOOLS_ERR_INVALID;
      }
      if (t->max_len >= 0 && (int64_t)len > t->max_len) {
        tset_err(err, err_cap, "%s: string length %zu exceeds max_len %lld",
                 ctx, len, (long long)t->max_len);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    }
    case AT_INTEGER: {
      int64_t iv;
      if (val->type != XCDN_VAL_INT)
        return mismatch(ctx, "integer", val, err, err_cap);
      iv = val->data.integer;
      if (t->has_min && !int_ge(iv, t->min_num)) {
        tset_err(err, err_cap, "%s: %lld is below minimum %g", ctx,
                 (long long)iv, t->min_num);
        return ASTOOLS_ERR_INVALID;
      }
      if (t->has_max && !int_le(iv, t->max_num)) {
        tset_err(err, err_cap, "%s: %lld exceeds maximum %g", ctx,
                 (long long)iv, t->max_num);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    }
    case AT_NUMBER: {
      double d;
      if (val->type == XCDN_VAL_INT) {
        d = (double)val->data.integer;
      } else if (val->type == XCDN_VAL_FLOAT) {
        d = val->data.floating;
      } else {
        return mismatch(ctx, "number", val, err, err_cap);
      }
      if (!isfinite(d)) {
        tset_err(err, err_cap, "%s: number must be finite", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      if (t->has_min && d < t->min_num) {
        tset_err(err, err_cap, "%s: %g is below minimum %g", ctx, d,
                 t->min_num);
        return ASTOOLS_ERR_INVALID;
      }
      if (t->has_max && d > t->max_num) {
        tset_err(err, err_cap, "%s: %g exceeds maximum %g", ctx, d,
                 t->max_num);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    }
    case AT_BOOLEAN:
      if (val->type != XCDN_VAL_BOOL)
        return mismatch(ctx, "boolean", val, err, err_cap);
      return ASTOOLS_OK;
    case AT_BYTES: {
      int64_t blen;
      if (val->type != XCDN_VAL_BYTES)
        return mismatch(ctx, "bytes", val, err, err_cap);
      blen = (int64_t)val->data.bytes.len;
      if (t->min_len >= 0 && blen < t->min_len) {
        tset_err(err, err_cap,
                 "%s: %lld decoded bytes are below min_len %lld", ctx,
                 (long long)blen, (long long)t->min_len);
        return ASTOOLS_ERR_INVALID;
      }
      if (t->max_len >= 0 && blen > t->max_len) {
        tset_err(err, err_cap, "%s: %lld decoded bytes exceed max_len %lld",
                 ctx, (long long)blen, (long long)t->max_len);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    }
    case AT_DATETIME: {
      astools_time when;
      if (val->type != XCDN_VAL_DATETIME)
        return mismatch(ctx, "datetime", val, err, err_cap);
      if (!val->data.string ||
          !astools_time_parse_rfc3339(val->data.string, &when)) {
        tset_err(err, err_cap, "%s: invalid RFC 3339 datetime", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    }
    case AT_DURATION: {
      int64_t secs;
      if (val->type != XCDN_VAL_DURATION)
        return mismatch(ctx, "duration", val, err, err_cap);
      if (!val->data.string ||
          !astools_duration_parse(val->data.string, &secs)) {
        tset_err(err, err_cap, "%s: invalid ISO 8601 duration", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    }
    case AT_UUID:
      if (val->type != XCDN_VAL_UUID)
        return mismatch(ctx, "uuid", val, err, err_cap);
      if (!val->data.string || !astools_uuid_valid(val->data.string)) {
        tset_err(err, err_cap, "%s: invalid UUID", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    case AT_PATH:
      /* Schema check only; resolution + grants happen in policy. */
      if (val->type != XCDN_VAL_STRING)
        return mismatch(ctx, "path (string)", val, err, err_cap);
      if (!val->data.string || val->data.string[0] == '\0') {
        tset_err(err, err_cap, "%s: path must be a non-empty string", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      return ASTOOLS_OK;
    case AT_ENUM: {
      size_t i;
      if (val->type != XCDN_VAL_STRING || !val->data.string)
        return mismatch(ctx, "enum (string)", val, err, err_cap);
      for (i = 0; i < t->values_len; i++)
        if (t->values[i] && strcmp(t->values[i], val->data.string) == 0)
          return ASTOOLS_OK;
      tset_err(err, err_cap, "%s: \"%.64s\" is not a permitted enum value",
               ctx, val->data.string);
      return ASTOOLS_ERR_INVALID;
    }
    case AT_ARRAY: {
      size_t i, n;
      if (val->type != XCDN_VAL_ARRAY)
        return mismatch(ctx, "array", val, err, err_cap);
      if (!t->item) {
        tset_err(err, err_cap, "%s: internal: array without item type",
                 ctx);
        return ASTOOLS_ERR_INVALID;
      }
      n = val->data.array.len;
      if (t->min_items >= 0 && (int64_t)n < t->min_items) {
        tset_err(err, err_cap, "%s: %zu items are below min_items %lld",
                 ctx, n, (long long)t->min_items);
        return ASTOOLS_ERR_INVALID;
      }
      if (t->max_items >= 0 && (int64_t)n > t->max_items) {
        tset_err(err, err_cap, "%s: %zu items exceed max_items %lld", ctx,
                 n, (long long)t->max_items);
        return ASTOOLS_ERR_INVALID;
      }
      for (i = 0; i < n; i++) {
        char cbuf[224];
        snprintf(cbuf, sizeof cbuf, "%s[%zu]", ctx, i);
        e = astools_type_check(t->item, val->data.array.items[i], cbuf,
                               err, err_cap);
        if (e != ASTOOLS_OK) return e;
      }
      return ASTOOLS_OK;
    }
    case AT_MAP: {
      size_t i, n;
      if (val->type != XCDN_VAL_OBJECT)
        return mismatch(ctx, "map (object)", val, err, err_cap);
      if (!t->value) {
        tset_err(err, err_cap, "%s: internal: map without value type", ctx);
        return ASTOOLS_ERR_INVALID;
      }
      n = val->data.object.len;
      if (t->max_entries >= 0 && (int64_t)n > t->max_entries) {
        tset_err(err, err_cap, "%s: %zu entries exceed max_entries %lld",
                 ctx, n, (long long)t->max_entries);
        return ASTOOLS_ERR_INVALID;
      }
      e = check_obj_dups(val, ctx, err, err_cap);
      if (e != ASTOOLS_OK) return e;
      for (i = 0; i < n; i++) {
        const char *k = val->data.object.entries[i].key;
        char cbuf[224];
        if (!k) {
          tset_err(err, err_cap, "%s: invalid map key", ctx);
          return ASTOOLS_ERR_INVALID;
        }
        snprintf(cbuf, sizeof cbuf, "%.110s.%.110s", ctx, k);
        e = astools_type_check(t->value, val->data.object.entries[i].node,
                               cbuf, err, err_cap);
        if (e != ASTOOLS_OK) return e;
      }
      return ASTOOLS_OK;
    }
    case AT_OBJECT: {
      size_t i, j;
      if (val->type != XCDN_VAL_OBJECT)
        return mismatch(ctx, "object", val, err, err_cap);
      e = check_obj_dups(val, ctx, err, err_cap);
      if (e != ASTOOLS_OK) return e;
      /* Closed field set: any unknown key is an error naming the key. */
      for (i = 0; i < val->data.object.len; i++) {
        const char *k = val->data.object.entries[i].key;
        bool known = false;
        if (!k) {
          tset_err(err, err_cap, "%s: invalid object key", ctx);
          return ASTOOLS_ERR_INVALID;
        }
        for (j = 0; j < t->fields_len; j++)
          if (t->fields[j].name && strcmp(t->fields[j].name, k) == 0) {
            known = true;
            break;
          }
        if (!known) {
          tset_err(err, err_cap, "%s: unknown field \"%.64s\"", ctx, k);
          return ASTOOLS_ERR_INVALID;
        }
      }
      for (j = 0; j < t->fields_len; j++) {
        const astools_param *p = &t->fields[j];
        xcdn_node_t *fnode;
        char cbuf[224];
        if (!p->name || !p->type) {
          tset_err(err, err_cap, "%s: internal: malformed field", ctx);
          return ASTOOLS_ERR_INVALID;
        }
        fnode = xcdn_object_get(val, p->name);
        if (fnode) {
          snprintf(cbuf, sizeof cbuf, "%.110s.%.110s", ctx, p->name);
          e = astools_type_check(p->type, fnode, cbuf, err, err_cap);
          if (e != ASTOOLS_OK) return e;
        } else if (p->required) {
          tset_err(err, err_cap, "%s: missing required field \"%s\"", ctx,
                   p->name);
          return ASTOOLS_ERR_INVALID;
        } else if (p->dflt) {
          /* Inject the default so nested objects are canonical too.
           * v is const-qualified but the pointed-to value is mutable;
           * the contract requires this mutation. */
          e = obj_inject_default(v->value, p->name, p->dflt);
          if (e != ASTOOLS_OK) {
            tset_err(err, err_cap,
                     "%s: cannot inject default for \"%s\"", ctx, p->name);
            return e;
          }
        }
      }
      return ASTOOLS_OK;
    }
    default:
      break;
  }
  tset_err(err, err_cap, "%s: internal: bad type kind", ctx);
  return ASTOOLS_ERR_INVALID;
}

/* ---- args validation (step 2) --------------------------------------- */

astools_err astools_args_validate(const astools_cmd *cmd, xcdn_node_t *args,
                                  char *err, size_t err_cap) {
  const xcdn_value_t *av;
  const char *cname;
  astools_err e;
  size_t i, j;

  if (err && err_cap > 0) err[0] = '\0';
  if (!cmd || (cmd->params_len > 0 && !cmd->params)) {
    tset_err(err, err_cap, "args: internal: missing command schema");
    return ASTOOLS_ERR_INVALID;
  }
  cname = cmd->name ? cmd->name : "?";
  if (!args || !args->value || args->value->type != XCDN_VAL_OBJECT) {
    tset_err(err, err_cap, "args: expected object, got %s",
             vt_name(args ? args->value : NULL));
    return ASTOOLS_ERR_INVALID;
  }
  av = args->value;

  e = check_obj_dups(av, "args", err, err_cap);
  if (e != ASTOOLS_OK) return e;

  for (i = 0; i < av->data.object.len; i++) {
    const char *k = av->data.object.entries[i].key;
    bool known = false;
    if (!k) {
      tset_err(err, err_cap, "args: invalid key in args object");
      return ASTOOLS_ERR_INVALID;
    }
    for (j = 0; j < cmd->params_len; j++)
      if (cmd->params[j].name && strcmp(cmd->params[j].name, k) == 0) {
        known = true;
        break;
      }
    if (!known) {
      tset_err(err, err_cap, "args: unknown key \"%.64s\" for command "
               "\"%s\"", k, cname);
      return ASTOOLS_ERR_INVALID;
    }
  }

  for (j = 0; j < cmd->params_len; j++) {
    const astools_param *p = &cmd->params[j];
    xcdn_node_t *n;
    char cbuf[160];
    if (!p->name || !p->type) {
      tset_err(err, err_cap, "args: internal: malformed param schema");
      return ASTOOLS_ERR_INVALID;
    }
    n = xcdn_object_get(av, p->name);
    if (n) {
      snprintf(cbuf, sizeof cbuf, "args.%s", p->name);
      e = astools_type_check(p->type, n, cbuf, err, err_cap);
      if (e != ASTOOLS_OK) return e;
    } else if (p->required) {
      tset_err(err, err_cap,
               "args: missing required parameter \"%s\" for command "
               "\"%s\"", p->name, cname);
      return ASTOOLS_ERR_INVALID;
    } else if (p->dflt) {
      e = obj_inject_default(args->value, p->name, p->dflt);
      if (e != ASTOOLS_OK) {
        tset_err(err, err_cap,
                 "args.%s: cannot inject default (out of memory)",
                 p->name);
        return e;
      }
    }
  }
  return ASTOOLS_OK;
}
