/*
 * proto.c — wire protocol: #tool_request / #tool_response / #tool_hello /
 * #tool_cancel build and parse (§5.2, §5.3, Appendix B).
 *
 * All tool output is hostile input: every shape is checked before use and
 * nothing is written into caller structures until the whole message has
 * been accepted. Builders go through checked growth helpers because the
 * xcdn-c mutators grow silently and would write out of bounds after a
 * failed realloc.
 */

#include "astools_internal.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- small allocation helpers ------------------------------------------- */

static char *vmsg_printf(const char *fmt, va_list ap) {
  va_list ap2;
  int need;
  char *p;
  va_copy(ap2, ap);
  need = vsnprintf(NULL, 0, fmt, ap2);
  va_end(ap2);
  if (need < 0) return NULL;
  p = malloc((size_t)need + 1);
  if (!p) return NULL;
  vsnprintf(p, (size_t)need + 1, fmt, ap);
  return p;
}

static char *msg_printf(const char *fmt, ...) {
  va_list ap;
  char *p;
  va_start(ap, fmt);
  p = vmsg_printf(fmt, ap);
  va_end(ap);
  return p;
}

/* Set *err_msg (first failure wins) and return ASTOOLS_ERR_PROTOCOL. */
static astools_err proto_fail(char **err_msg, const char *fmt, ...) {
  va_list ap;
  if (err_msg && !*err_msg) {
    va_start(ap, fmt);
    *err_msg = vmsg_printf(fmt, ap);
    va_end(ap);
  }
  return ASTOOLS_ERR_PROTOCOL;
}

/* ---- checked growth over the transparent xcdn structs -------------------
 * Returns the (possibly moved) items pointer with room for one more
 * element, or NULL on overflow/OOM; the original allocation is untouched
 * on failure. */
static void *grow_checked(void *items, size_t len, size_t *cap,
                          size_t elem_size) {
  size_t ncap;
  void *np;
  if (len < *cap) return items;
  ncap = *cap ? *cap * 2 : 4;
  if (ncap > SIZE_MAX / elem_size) return NULL;
  np = realloc(items, ncap * elem_size);
  if (!np) return NULL;
  *cap = ncap;
  return np;
}

static astools_err doc_push_checked(xcdn_document_t *doc, xcdn_node_t *n) {
  xcdn_node_t **items;
  items = grow_checked(doc->values, doc->values_len, &doc->values_cap,
                       sizeof *items);
  if (!items) return ASTOOLS_ERR_NOMEM;
  doc->values = items;
  items[doc->values_len++] = n;
  return ASTOOLS_OK;
}

static astools_err arr_push_checked(xcdn_value_t *arr, xcdn_node_t *n) {
  xcdn_node_t **items;
  if (!arr || arr->type != XCDN_VAL_ARRAY) return ASTOOLS_ERR_INVALID;
  items = grow_checked(arr->data.array.items, arr->data.array.len,
                       &arr->data.array.cap, sizeof *items);
  if (!items) return ASTOOLS_ERR_NOMEM;
  arr->data.array.items = items;
  items[arr->data.array.len++] = n;
  return ASTOOLS_OK;
}

/* Append-only set: keys we emit are distinct, and clones must preserve
 * source entries verbatim (duplicates included). */
static astools_err obj_set_checked(xcdn_value_t *obj, const char *key,
                                   xcdn_node_t *n) {
  xcdn_object_entry_t *entries;
  char *k;
  if (!obj || obj->type != XCDN_VAL_OBJECT) return ASTOOLS_ERR_INVALID;
  entries = grow_checked(obj->data.object.entries, obj->data.object.len,
                         &obj->data.object.cap, sizeof *entries);
  if (!entries) return ASTOOLS_ERR_NOMEM;
  obj->data.object.entries = entries;
  k = astools_strdup(key ? key : "");
  if (!k) return ASTOOLS_ERR_NOMEM;
  entries[obj->data.object.len].key = k;
  entries[obj->data.object.len].node = n;
  obj->data.object.len++;
  return ASTOOLS_OK;
}

static astools_err node_add_tag_checked(xcdn_node_t *n, const char *name) {
  xcdn_tag_t *tags;
  char *dup;
  tags = grow_checked(n->tags, n->tags_len, &n->tags_cap, sizeof *tags);
  if (!tags) return ASTOOLS_ERR_NOMEM;
  n->tags = tags;
  dup = astools_strdup(name);
  if (!dup) return ASTOOLS_ERR_NOMEM;
  tags[n->tags_len].name = dup;
  n->tags_len++;
  return ASTOOLS_OK;
}

/* ---- deep clone ---------------------------------------------------------
 * The serializer only accepts whole documents and the document destructor
 * frees every reachable node, so borrowed nodes are never pushed into a
 * temp document — a full clone is. */

static xcdn_node_t *clone_node(const xcdn_node_t *n);

static xcdn_value_t *clone_string_like(const xcdn_value_t *v) {
  xcdn_value_t *c;
  switch (v->type) {
  case XCDN_VAL_DECIMAL:
    c = xcdn_value_decimal(v->data.string);
    break;
  case XCDN_VAL_STRING:
    c = xcdn_value_string(v->data.string);
    break;
  case XCDN_VAL_DATETIME:
    c = xcdn_value_datetime(v->data.string);
    break;
  case XCDN_VAL_DURATION:
    c = xcdn_value_duration(v->data.string);
    break;
  default: /* XCDN_VAL_UUID */
    c = xcdn_value_uuid(v->data.string);
    break;
  }
  /* Constructors swallow a failed strdup; detect it. */
  if (c && v->data.string && !c->data.string) {
    xcdn_value_free(c);
    return NULL;
  }
  return c;
}

static xcdn_value_t *clone_value(const xcdn_value_t *v) {
  xcdn_value_t *c;
  size_t i;
  if (!v) return xcdn_value_null(); /* serializer renders both as null */
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
  case XCDN_VAL_UUID:
    return clone_string_like(v);
  case XCDN_VAL_BYTES:
    if (!v->data.bytes.data || v->data.bytes.len == 0)
      return xcdn_value_bytes_owned(NULL, 0);
    c = xcdn_value_bytes(v->data.bytes.data, v->data.bytes.len);
    if (c && !c->data.bytes.data) {
      xcdn_value_free(c);
      return NULL;
    }
    return c;
  case XCDN_VAL_ARRAY:
    c = xcdn_value_array();
    if (!c) return NULL;
    for (i = 0; i < v->data.array.len; i++) {
      xcdn_node_t *cn = clone_node(v->data.array.items[i]);
      if (!cn || arr_push_checked(c, cn) != ASTOOLS_OK) {
        xcdn_node_free(cn);
        xcdn_value_free(c);
        return NULL;
      }
    }
    return c;
  case XCDN_VAL_OBJECT:
    c = xcdn_value_object();
    if (!c) return NULL;
    for (i = 0; i < v->data.object.len; i++) {
      xcdn_node_t *cn = clone_node(v->data.object.entries[i].node);
      if (!cn ||
          obj_set_checked(c, v->data.object.entries[i].key, cn) !=
              ASTOOLS_OK) {
        xcdn_node_free(cn);
        xcdn_value_free(c);
        return NULL;
      }
    }
    return c;
  default:
    return NULL;
  }
}

/* Clone one annotation into dst (zeroed slot). On failure frees whatever
 * was built into dst and returns NOMEM. */
static astools_err clone_annotation(xcdn_annotation_t *dst,
                                    const xcdn_annotation_t *src) {
  size_t i;
  memset(dst, 0, sizeof *dst);
  dst->name = astools_strdup(src->name ? src->name : "");
  if (!dst->name) return ASTOOLS_ERR_NOMEM;
  for (i = 0; i < src->args_len; i++) {
    xcdn_value_t **args;
    xcdn_value_t *cv = clone_value(src->args[i]);
    if (!cv) goto fail;
    args = grow_checked(dst->args, dst->args_len, &dst->args_cap,
                        sizeof *args);
    if (!args) {
      xcdn_value_free(cv);
      goto fail;
    }
    dst->args = args;
    args[dst->args_len++] = cv;
  }
  return ASTOOLS_OK;
fail:
  free(dst->name);
  for (i = 0; i < dst->args_len; i++) xcdn_value_free(dst->args[i]);
  free(dst->args);
  memset(dst, 0, sizeof *dst);
  return ASTOOLS_ERR_NOMEM;
}

static xcdn_node_t *clone_node(const xcdn_node_t *n) {
  xcdn_node_t *c;
  xcdn_value_t *cv;
  size_t i;
  if (!n) return NULL;
  cv = clone_value(n->value);
  if (!cv) return NULL;
  c = xcdn_node_new(cv);
  if (!c) {
    xcdn_value_free(cv);
    return NULL;
  }
  for (i = 0; i < n->tags_len; i++) {
    if (!n->tags[i].name) continue;
    if (node_add_tag_checked(c, n->tags[i].name) != ASTOOLS_OK) {
      xcdn_node_free(c);
      return NULL;
    }
  }
  for (i = 0; i < n->annotations_len; i++) {
    xcdn_annotation_t *anns;
    anns = grow_checked(c->annotations, c->annotations_len,
                        &c->annotations_cap, sizeof *anns);
    if (!anns ||
        clone_annotation(&anns[c->annotations_len], &n->annotations[i]) !=
            ASTOOLS_OK) {
      if (anns) c->annotations = anns;
      xcdn_node_free(c);
      return NULL;
    }
    c->annotations = anns;
    c->annotations_len++;
  }
  return c;
}

/* ---- standalone serialization ------------------------------------------- */

static char *doc_to_text(xcdn_document_t *doc, bool pretty) {
  return pretty ? xcdn_to_string_pretty(doc) : xcdn_to_string_compact(doc);
}

/* Wrap an owned node in a temp document, serialize, free everything. */
static char *write_owned_node(xcdn_node_t *owned, bool pretty) {
  xcdn_document_t *doc;
  char *text;
  doc = xcdn_document_new();
  if (!doc) {
    xcdn_node_free(owned);
    return NULL;
  }
  if (doc_push_checked(doc, owned) != ASTOOLS_OK) {
    xcdn_node_free(owned);
    xcdn_document_free(doc);
    return NULL;
  }
  text = doc_to_text(doc, pretty);
  xcdn_document_free(doc);
  return text;
}

char *astools_xcdn_write_node(const xcdn_node_t *n, bool pretty) {
  xcdn_node_t *cl;
  if (!n) return NULL;
  cl = clone_node(n);
  if (!cl) return NULL;
  return write_owned_node(cl, pretty);
}

char *astools_xcdn_write_value(const xcdn_value_t *v, bool pretty) {
  xcdn_value_t *cv;
  xcdn_node_t *node;
  if (!v) return NULL;
  cv = clone_value(v);
  if (!cv) return NULL;
  node = xcdn_node_new(cv);
  if (!node) {
    xcdn_value_free(cv);
    return NULL;
  }
  return write_owned_node(node, pretty);
}

/* ---- parse first value --------------------------------------------------- */

astools_err astools_xcdn_parse_first(const char *text, size_t len,
                                     xcdn_document_t **out_doc,
                                     xcdn_node_t **out_node, char **err_msg) {
  xcdn_document_t *doc;
  xcdn_error_t perr;
  if (err_msg) *err_msg = NULL;
  if (!out_doc || !out_node) return ASTOOLS_ERR_INVALID;
  *out_doc = NULL;
  *out_node = NULL;
  if (!text) {
    if (err_msg) *err_msg = msg_printf("xcdn: no input");
    return ASTOOLS_ERR_PARSE;
  }
  doc = xcdn_parse_str(text, len, &perr);
  if (!doc) {
    if (err_msg)
      *err_msg = msg_printf("xcdn: parse error at line %zu, column %zu: %s",
                            perr.span.line, perr.span.column, perr.message);
    return ASTOOLS_ERR_PARSE;
  }
  if (doc->values_len == 0 || !doc->values[0]) {
    xcdn_document_free(doc);
    if (err_msg) *err_msg = msg_printf("xcdn: document has no values");
    return ASTOOLS_ERR_PARSE;
  }
  *out_doc = doc;
  *out_node = doc->values[0];
  return ASTOOLS_OK;
}

/* ---- request builder ----------------------------------------------------- */

static const char *access_str(int access) {
  switch (access) {
  case ASTOOLS_ACCESS_WRITE:
    return "write";
  case ASTOOLS_ACCESS_RW:
    return "read-write";
  default: /* READ and (defensively) anything else: understate */
    return "read";
  }
}

static xcdn_value_t *mk_string(const char *s) {
  xcdn_value_t *v = xcdn_value_string(s ? s : "");
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

static xcdn_value_t *mk_uuid(const char *s) {
  xcdn_value_t *v = xcdn_value_uuid(s);
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

static xcdn_value_t *mk_datetime(const char *s) {
  xcdn_value_t *v = xcdn_value_datetime(s);
  if (v && !v->data.string) {
    xcdn_value_free(v);
    return NULL;
  }
  return v;
}

/* Attach val to obj under key. Always consumes val (freed on failure). */
static astools_err obj_put(xcdn_value_t *obj, const char *key,
                           xcdn_value_t *val) {
  xcdn_node_t *n;
  if (!val) return ASTOOLS_ERR_NOMEM;
  n = xcdn_node_new(val);
  if (!n) {
    xcdn_value_free(val);
    return ASTOOLS_ERR_NOMEM;
  }
  if (obj_set_checked(obj, key, n) != ASTOOLS_OK) {
    xcdn_node_free(n);
    return ASTOOLS_ERR_NOMEM;
  }
  return ASTOOLS_OK;
}

/* Append val to arr. Always consumes val. */
static astools_err arr_put(xcdn_value_t *arr, xcdn_value_t *val) {
  xcdn_node_t *n;
  if (!val) return ASTOOLS_ERR_NOMEM;
  n = xcdn_node_new(val);
  if (!n) {
    xcdn_value_free(val);
    return ASTOOLS_ERR_NOMEM;
  }
  if (arr_push_checked(arr, n) != ASTOOLS_OK) {
    xcdn_node_free(n);
    return ASTOOLS_ERR_NOMEM;
  }
  return ASTOOLS_OK;
}

/* Serialize a document compact and append the protocol's trailing '\n'. */
static char *doc_compact_line(xcdn_document_t *doc) {
  char *body, *out;
  size_t blen;
  body = xcdn_to_string_compact(doc);
  if (!body) return NULL;
  blen = strlen(body);
  out = malloc(blen + 2);
  if (out) {
    memcpy(out, body, blen);
    out[blen] = '\n';
    out[blen + 1] = '\0';
  }
  free(body);
  return out;
}

astools_err astools_proto_request(const astools_tool *t,
                                  const astools_cmd *cmd,
                                  const xcdn_node_t *args,
                                  const char *invocation_id,
                                  astools_time deadline,
                                  int64_t max_output_bytes,
                                  const astools_effective *eff,
                                  const char *workspace, const char *scratch,
                                  char **out_text) {
  xcdn_value_t *root = NULL, *sub = NULL, *arr = NULL, *entry = NULL;
  xcdn_node_t *root_node = NULL, *args_clone = NULL;
  xcdn_document_t *doc = NULL;
  char stamp[32];
  char *out;
  size_t i;
  astools_err e = ASTOOLS_ERR_NOMEM;

  if (!out_text) return ASTOOLS_ERR_INVALID;
  *out_text = NULL;
  if (!t || !t->m || !t->m->id || !t->m->version || !cmd || !cmd->name ||
      !args || !invocation_id || !eff || !workspace || !scratch)
    return ASTOOLS_ERR_INVALID;

  root = xcdn_value_object();
  if (!root) goto done;

  if (obj_put(root, "protocol", xcdn_value_int(1)) != ASTOOLS_OK) goto done;
  if (obj_put(root, "invocation_id", mk_uuid(invocation_id)) != ASTOOLS_OK)
    goto done;
  if (obj_put(root, "tool", mk_string(t->m->id)) != ASTOOLS_OK) goto done;
  if (obj_put(root, "version", mk_string(t->m->version)) != ASTOOLS_OK)
    goto done;
  if (obj_put(root, "command", mk_string(cmd->name)) != ASTOOLS_OK) goto done;

  args_clone = clone_node(args);
  if (!args_clone) goto done;
  if (obj_set_checked(root, "args", args_clone) != ASTOOLS_OK) goto done;
  args_clone = NULL; /* owned by root now */

  astools_time_format_rfc3339(deadline, stamp);
  if (obj_put(root, "deadline", mk_datetime(stamp)) != ASTOOLS_OK) goto done;

  sub = xcdn_value_object(); /* limits */
  if (!sub) goto done;
  if (obj_put(sub, "max_output_bytes", xcdn_value_int(max_output_bytes)) !=
      ASTOOLS_OK)
    goto done;
  e = obj_put(root, "limits", sub);
  sub = NULL;
  if (e != ASTOOLS_OK) goto done;
  e = ASTOOLS_ERR_NOMEM;

  sub = xcdn_value_object(); /* grants */
  if (!sub) goto done;
  arr = xcdn_value_array(); /* grants.fs */
  if (!arr) goto done;
  for (i = 0; i < eff->fs_len; i++) {
    entry = xcdn_value_object();
    if (!entry) goto done;
    if (obj_put(entry, "path", mk_string(eff->fs[i].path)) != ASTOOLS_OK)
      goto done;
    if (obj_put(entry, "access", mk_string(access_str(eff->fs[i].access))) !=
        ASTOOLS_OK)
      goto done;
    e = arr_put(arr, entry);
    entry = NULL;
    if (e != ASTOOLS_OK) goto done;
    e = ASTOOLS_ERR_NOMEM;
  }
  e = obj_put(sub, "fs", arr);
  arr = NULL;
  if (e != ASTOOLS_OK) goto done;
  e = ASTOOLS_ERR_NOMEM;
  if (obj_put(sub, "net", xcdn_value_bool(eff->net)) != ASTOOLS_OK) goto done;
  if (obj_put(sub, "proc", xcdn_value_bool(eff->proc)) != ASTOOLS_OK)
    goto done;
  arr = xcdn_value_array(); /* grants.env */
  if (!arr) goto done;
  for (i = 0; i < eff->env_len; i++) {
    if (!eff->env[i]) continue;
    if (arr_put(arr, mk_string(eff->env[i])) != ASTOOLS_OK) goto done;
  }
  e = obj_put(sub, "env", arr);
  arr = NULL;
  if (e != ASTOOLS_OK) goto done;
  e = obj_put(root, "grants", sub);
  sub = NULL;
  if (e != ASTOOLS_OK) goto done;
  e = ASTOOLS_ERR_NOMEM;

  if (obj_put(root, "workspace", mk_string(workspace)) != ASTOOLS_OK)
    goto done;
  if (obj_put(root, "scratch", mk_string(scratch)) != ASTOOLS_OK) goto done;

  root_node = xcdn_node_new(root);
  if (!root_node) goto done;
  root = NULL; /* owned by root_node */
  if (node_add_tag_checked(root_node, "tool_request") != ASTOOLS_OK)
    goto done;

  doc = xcdn_document_new();
  if (!doc) goto done;
  if (doc_push_checked(doc, root_node) != ASTOOLS_OK) goto done;
  root_node = NULL; /* owned by doc */

  out = doc_compact_line(doc);
  if (!out) goto done;
  *out_text = out;
  e = ASTOOLS_OK;

done:
  xcdn_value_free(entry);
  xcdn_value_free(arr);
  xcdn_value_free(sub);
  xcdn_value_free(root);
  xcdn_node_free(args_clone);
  xcdn_node_free(root_node);
  xcdn_document_free(doc);
  return e;
}

/* ---- response parsing ---------------------------------------------------- */

/* Tag lookup that tolerates NULL tag names (hostile / OOM-damaged trees). */
static bool node_tagged(const xcdn_node_t *n, const char *name) {
  size_t i;
  if (!n) return false;
  for (i = 0; i < n->tags_len; i++)
    if (n->tags[i].name && strcmp(n->tags[i].name, name) == 0) return true;
  return false;
}

static bool node_int(const xcdn_node_t *n, int64_t *out) {
  if (!n || !n->value || n->value->type != XCDN_VAL_INT) return false;
  *out = n->value->data.integer;
  return true;
}

static bool node_bool(const xcdn_node_t *n, bool *out) {
  if (!n || !n->value || n->value->type != XCDN_VAL_BOOL) return false;
  *out = n->value->data.boolean;
  return true;
}

/* STRING only; NULL when absent/mistyped. */
static const char *node_string(const xcdn_node_t *n) {
  if (!n || !n->value || n->value->type != XCDN_VAL_STRING) return NULL;
  return n->value->data.string;
}

/* invocation_id may arrive as u"…" or a plain string. */
static const char *node_id_string(const xcdn_node_t *n) {
  if (!n || !n->value) return NULL;
  if (n->value->type != XCDN_VAL_UUID && n->value->type != XCDN_VAL_STRING)
    return NULL;
  return n->value->data.string;
}

astools_err astools_proto_parse_response(const char *text, size_t len,
                                         const char *expect_id,
                                         char **out_id, astools_result *r,
                                         char **err_msg) {
  xcdn_document_t *doc = NULL;
  const xcdn_node_t *resp = NULL;
  const xcdn_value_t *obj;
  const xcdn_node_t *fn;
  xcdn_error_t perr;
  const char *id;
  int64_t proto;
  bool okval = false;
  char *result_text = NULL, *code_copy = NULL, *msg_copy = NULL,
       *id_copy = NULL;
  astools_err e;
  size_t i;

  if (err_msg) *err_msg = NULL;
  if (out_id) *out_id = NULL;
  if (r) memset(r, 0, sizeof *r);
  if (!r || !text) return ASTOOLS_ERR_INVALID;

  doc = xcdn_parse_str(text, len, &perr);
  if (!doc)
    return proto_fail(err_msg, "tool_response: parse error: %s",
                      perr.message);

  /* First top-level #tool_response wins; other values are skipped. */
  for (i = 0; i < doc->values_len; i++) {
    if (node_tagged(doc->values[i], "tool_response")) {
      resp = doc->values[i];
      break;
    }
  }
  if (!resp) {
    e = proto_fail(err_msg, "tool output contains no #tool_response");
    goto done;
  }

  obj = resp->value;
  if (!obj || obj->type != XCDN_VAL_OBJECT) {
    e = proto_fail(err_msg, "tool_response: value is not an object");
    goto done;
  }

  fn = xcdn_object_get(obj, "protocol");
  if (!node_int(fn, &proto) || proto != 1) {
    e = proto_fail(err_msg, "tool_response: protocol must be the integer 1");
    goto done;
  }

  fn = xcdn_object_get(obj, "invocation_id");
  id = node_id_string(fn);
  if (!id || id[0] == '\0') {
    e = proto_fail(err_msg,
                   "tool_response: missing or invalid invocation_id");
    goto done;
  }
  if (expect_id && strcmp(id, expect_id) != 0) {
    e = proto_fail(err_msg,
                   "tool_response: invocation_id \"%s\" does not match "
                   "expected \"%s\"",
                   id, expect_id);
    goto done;
  }

  fn = xcdn_object_get(obj, "ok");
  if (!node_bool(fn, &okval)) {
    e = proto_fail(err_msg, "tool_response: 'ok' must be a boolean");
    goto done;
  }

  if (okval) {
    fn = xcdn_object_get(obj, "result");
    if (!fn) {
      e = proto_fail(err_msg, "tool_response: ok=true requires 'result'");
      goto done;
    }
    result_text = astools_xcdn_write_node(fn, false);
    if (!result_text) {
      e = ASTOOLS_ERR_NOMEM;
      goto done;
    }
  } else {
    const xcdn_node_t *en = xcdn_object_get(obj, "error");
    const xcdn_value_t *eo;
    const char *code, *msg;
    bool retriable;
    if (!en || !en->value || en->value->type != XCDN_VAL_OBJECT) {
      e = proto_fail(err_msg,
                     "tool_response: ok=false requires an 'error' object");
      goto done;
    }
    eo = en->value;
    code = node_string(xcdn_object_get(eo, "code"));
    if (!code || code[0] == '\0') {
      e = proto_fail(err_msg,
                     "tool_response: error.code must be a non-empty string");
      goto done;
    }
    msg = node_string(xcdn_object_get(eo, "message"));
    if (!msg) {
      e = proto_fail(err_msg,
                     "tool_response: error.message must be a string");
      goto done;
    }
    fn = xcdn_object_get(eo, "retriable");
    if (fn && !node_bool(fn, &retriable)) {
      e = proto_fail(err_msg,
                     "tool_response: error.retriable must be a boolean");
      goto done;
    }
    code_copy = astools_strdup(code);
    msg_copy = astools_strdup(msg);
    if (!code_copy || !msg_copy) {
      e = ASTOOLS_ERR_NOMEM;
      goto done;
    }
  }

  if (out_id) {
    id_copy = astools_strdup(id);
    if (!id_copy) {
      e = ASTOOLS_ERR_NOMEM;
      goto done;
    }
  }

  /* Whole message accepted — hand everything over. */
  r->ok = okval ? 1 : 0;
  r->result_xcdn = result_text;
  result_text = NULL;
  r->error_code = code_copy;
  code_copy = NULL;
  r->error_message = msg_copy;
  msg_copy = NULL;
  if (out_id) {
    *out_id = id_copy;
    id_copy = NULL;
  }
  e = ASTOOLS_OK;

done:
  free(result_text);
  free(code_copy);
  free(msg_copy);
  free(id_copy);
  xcdn_document_free(doc);
  return e;
}

/* ---- persistent-mode messages (§5.3) ------------------------------------- */

astools_err astools_proto_parse_hello(const char *text, size_t len,
                                      const astools_tool *t, char **err_msg) {
  xcdn_document_t *doc;
  const xcdn_node_t *hello = NULL;
  const xcdn_value_t *obj;
  xcdn_error_t perr;
  const char *s;
  int64_t iv;
  astools_err e;
  size_t i;

  if (err_msg) *err_msg = NULL;
  if (!text || !t || !t->m || !t->m->id || !t->m->version)
    return ASTOOLS_ERR_INVALID;

  doc = xcdn_parse_str(text, len, &perr);
  if (!doc)
    return proto_fail(err_msg, "tool_hello: parse error: %s", perr.message);

  for (i = 0; i < doc->values_len; i++) {
    if (node_tagged(doc->values[i], "tool_hello")) {
      hello = doc->values[i];
      break;
    }
  }
  if (!hello) {
    e = proto_fail(err_msg, "tool output contains no #tool_hello");
    goto done;
  }
  obj = hello->value;
  if (!obj || obj->type != XCDN_VAL_OBJECT) {
    e = proto_fail(err_msg, "tool_hello: value is not an object");
    goto done;
  }
  if (!node_int(xcdn_object_get(obj, "protocol"), &iv) || iv != 1) {
    e = proto_fail(err_msg, "tool_hello: protocol must be the integer 1");
    goto done;
  }
  s = node_string(xcdn_object_get(obj, "tool"));
  if (!s || strcmp(s, t->m->id) != 0) {
    e = proto_fail(err_msg, "tool_hello: tool \"%s\" does not match \"%s\"",
                   s ? s : "(missing)", t->m->id);
    goto done;
  }
  s = node_string(xcdn_object_get(obj, "version"));
  if (!s || strcmp(s, t->m->version) != 0) {
    e = proto_fail(err_msg,
                   "tool_hello: version \"%s\" does not match \"%s\"",
                   s ? s : "(missing)", t->m->version);
    goto done;
  }
  if (!node_int(xcdn_object_get(obj, "parallel"), &iv) || iv < 1) {
    e = proto_fail(err_msg, "tool_hello: parallel must be an integer >= 1");
    goto done;
  }
  e = ASTOOLS_OK;

done:
  xcdn_document_free(doc);
  return e;
}

char *astools_proto_cancel(const char *invocation_id) {
  xcdn_value_t *root = NULL;
  xcdn_node_t *node = NULL;
  xcdn_document_t *doc = NULL;
  char *out = NULL;

  /* Ids are engine-generated v4 UUIDs; anything else is a caller bug and
   * must not reach the wire inside a u"…" literal. */
  if (!invocation_id || !astools_uuid_valid(invocation_id)) return NULL;

  root = xcdn_value_object();
  if (!root) return NULL;
  if (obj_put(root, "invocation_id", mk_uuid(invocation_id)) != ASTOOLS_OK)
    goto done;
  node = xcdn_node_new(root);
  if (!node) goto done;
  root = NULL; /* owned by node */
  if (node_add_tag_checked(node, "tool_cancel") != ASTOOLS_OK) goto done;
  doc = xcdn_document_new();
  if (!doc) goto done;
  if (doc_push_checked(doc, node) != ASTOOLS_OK) goto done;
  node = NULL; /* owned by doc */
  out = doc_compact_line(doc);

done:
  xcdn_value_free(root);
  xcdn_node_free(node);
  xcdn_document_free(doc);
  return out;
}
