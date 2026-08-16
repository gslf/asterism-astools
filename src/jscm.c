/*
 * jscm.c — JSON Schema + MCP description strings.
 *
 * Emits JSON as text by hand into astools_buf (libastools links no JSON
 * library). All renderings are deterministic: key order is fixed, numbers
 * use the shortest round-trip form, and required arrays follow manifest
 * order. Inputs originate from parsed manifests, but every pointer is
 * treated as potentially absent (hostile-manifest defensive posture).
 */

#include "astools_internal.h"

#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

/* Manifest types are trees of bounded size; the cap only guards against a
 * pathological parser bug feeding us a cyclic/degenerate structure. */
#define JSCM_MAX_DEPTH 64

/* ---- JSON primitives ---------------------------------------------------- */

/* Append one RFC 8259 string literal (quotes included). '"', '\\' and
 * control characters are escaped (\u00xx); UTF-8 bytes pass through. */
static astools_err json_escape(astools_buf *b, const char *s) {
  const unsigned char *p;
  astools_err e;
  e = astools_buf_appendc(b, '"');
  if (e != ASTOOLS_OK) return e;
  if (!s) s = "";
  for (p = (const unsigned char *)s; *p != '\0'; p++) {
    unsigned char ch = *p;
    if (ch == '"')
      e = astools_buf_appends(b, "\\\"");
    else if (ch == '\\')
      e = astools_buf_appends(b, "\\\\");
    else if (ch < 0x20)
      e = astools_buf_printf(b, "\\u%04x", (unsigned)ch);
    else
      e = astools_buf_appendc(b, (char)ch);
    if (e != ASTOOLS_OK) return e;
  }
  return astools_buf_appendc(b, '"');
}

/* Shortest %g form that round-trips through strtod; deterministic.
 * Non-finite values have no JSON representation and degrade to null. */
static astools_err json_double(astools_buf *b, double d) {
  char tmp[40];
  int prec;
  if (d != d || d > DBL_MAX || d < -DBL_MAX)
    return astools_buf_appends(b, "null");
  for (prec = 1; prec < 17; prec++) {
    snprintf(tmp, sizeof(tmp), "%.*g", prec, d);
    if (strtod(tmp, NULL) == d) return astools_buf_appends(b, tmp);
  }
  snprintf(tmp, sizeof(tmp), "%.17g", d);
  return astools_buf_appends(b, tmp);
}

/* Numeric bound: integer-typed bounds render as JSON integers when the
 * stored double is integral and fits int64. */
static astools_err json_number(astools_buf *b, double d, bool as_int) {
  if (as_int && d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
    int64_t i = (int64_t)d;
    if ((double)i == d) return astools_buf_printf(b, "%" PRId64, i);
  }
  return json_double(b, d);
}

/* ---- xCDN default value -> JSON ----------------------------------------- */

/* datetime/duration/uuid/decimal defaults render as JSON strings of their
 * textual form; bytes as base64. */
static astools_err default_json(astools_buf *b, const xcdn_node_t *n,
                                int depth) {
  const xcdn_value_t *v;
  astools_err e;
  size_t i;
  if (depth > JSCM_MAX_DEPTH) return ASTOOLS_ERR_INVALID;
  if (!n || !n->value) return astools_buf_appends(b, "null");
  v = n->value;
  switch (v->type) {
  case XCDN_VAL_NULL:
    return astools_buf_appends(b, "null");
  case XCDN_VAL_BOOL:
    return astools_buf_appends(b, v->data.boolean ? "true" : "false");
  case XCDN_VAL_INT:
    return astools_buf_printf(b, "%" PRId64, v->data.integer);
  case XCDN_VAL_FLOAT:
    return json_double(b, v->data.floating);
  case XCDN_VAL_DECIMAL:
  case XCDN_VAL_STRING:
  case XCDN_VAL_DATETIME:
  case XCDN_VAL_DURATION:
  case XCDN_VAL_UUID:
    return json_escape(b, v->data.string);
  case XCDN_VAL_BYTES: {
    char *b64 = astools_base64_encode(v->data.bytes.data, v->data.bytes.len);
    if (!b64) return ASTOOLS_ERR_NOMEM;
    e = json_escape(b, b64);
    free(b64);
    return e;
  }
  case XCDN_VAL_ARRAY:
    e = astools_buf_appendc(b, '[');
    if (e != ASTOOLS_OK) return e;
    for (i = 0; i < v->data.array.len; i++) {
      if (i > 0) {
        e = astools_buf_appendc(b, ',');
        if (e != ASTOOLS_OK) return e;
      }
      e = default_json(b, v->data.array.items[i], depth + 1);
      if (e != ASTOOLS_OK) return e;
    }
    return astools_buf_appendc(b, ']');
  case XCDN_VAL_OBJECT:
    e = astools_buf_appendc(b, '{');
    if (e != ASTOOLS_OK) return e;
    for (i = 0; i < v->data.object.len; i++) {
      if (i > 0) {
        e = astools_buf_appendc(b, ',');
        if (e != ASTOOLS_OK) return e;
      }
      e = json_escape(b, v->data.object.entries[i].key);
      if (e != ASTOOLS_OK) return e;
      e = astools_buf_appendc(b, ':');
      if (e != ASTOOLS_OK) return e;
      e = default_json(b, v->data.object.entries[i].node, depth + 1);
      if (e != ASTOOLS_OK) return e;
    }
    return astools_buf_appendc(b, '}');
  }
  return astools_buf_appends(b, "null");
}

/* ---- type -> schema ------------------------------------------------------ */

static const char *json_type_name(astools_tkind kind) {
  switch (kind) {
  case AT_INTEGER:
    return "integer";
  case AT_NUMBER:
    return "number";
  case AT_BOOLEAN:
    return "boolean";
  case AT_ARRAY:
    return "array";
  case AT_MAP:
  case AT_OBJECT:
    return "object";
  default: /* string, path, bytes, datetime, duration, uuid */
    return "string";
  }
}

static const char *path_access_str(int access) {
  if ((access & ASTOOLS_ACCESS_RW) == ASTOOLS_ACCESS_RW) return "read-write";
  if (access & ASTOOLS_ACCESS_WRITE) return "write";
  return "read";
}

/* ",\"description\":\"...\"" — for AT_PATH the path semantics are appended
 * to the param description (or stand alone when there is none). */
static astools_err emit_description(astools_buf *b, const astools_type *t,
                                    const char *desc) {
  astools_buf tmp;
  astools_err e;
  bool have = desc != NULL && desc[0] != '\0';
  if (t->kind != AT_PATH) {
    if (!have) return ASTOOLS_OK;
    e = astools_buf_appends(b, ",\"description\":");
    if (e != ASTOOLS_OK) return e;
    return json_escape(b, desc);
  }
  astools_buf_init(&tmp);
  e = ASTOOLS_OK;
  if (have) {
    e = astools_buf_appends(&tmp, desc);
    if (e == ASTOOLS_OK) e = astools_buf_appends(&tmp, " (");
  }
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&tmp, "workspace-relative path; access: ");
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&tmp, path_access_str(t->access));
  if (e == ASTOOLS_OK && t->must_exist)
    e = astools_buf_appends(&tmp, ", must exist");
  if (e == ASTOOLS_OK && have) e = astools_buf_appendc(&tmp, ')');
  if (e == ASTOOLS_OK) e = astools_buf_appends(b, ",\"description\":");
  if (e == ASTOOLS_OK) e = json_escape(b, tmp.data != NULL ? tmp.data : "");
  astools_buf_free(&tmp);
  return e;
}

static astools_err type_schema(astools_buf *b, const astools_type *t,
                               const char *desc, const xcdn_node_t *dflt,
                               int depth);

/* "properties":{...},"required":[...],"additionalProperties":false —
 * shared by the top-level command schema and nested AT_OBJECT types.
 * "required" is always emitted, even when empty (MCP expects it). */
static astools_err object_body(astools_buf *b, const astools_param *params,
                               size_t n, int depth) {
  astools_err e;
  size_t i;
  bool first;
  if (depth > JSCM_MAX_DEPTH) return ASTOOLS_ERR_INVALID;
  if (!params) n = 0;
  e = astools_buf_appends(b, "\"properties\":{");
  if (e != ASTOOLS_OK) return e;
  for (i = 0; i < n; i++) {
    if (i > 0) {
      e = astools_buf_appendc(b, ',');
      if (e != ASTOOLS_OK) return e;
    }
    e = json_escape(b, params[i].name);
    if (e != ASTOOLS_OK) return e;
    e = astools_buf_appendc(b, ':');
    if (e != ASTOOLS_OK) return e;
    e = type_schema(b, params[i].type, params[i].description, params[i].dflt,
                    depth + 1);
    if (e != ASTOOLS_OK) return e;
  }
  e = astools_buf_appends(b, "},\"required\":[");
  if (e != ASTOOLS_OK) return e;
  first = true;
  for (i = 0; i < n; i++) {
    if (!params[i].required) continue;
    if (!first) {
      e = astools_buf_appendc(b, ',');
      if (e != ASTOOLS_OK) return e;
    }
    e = json_escape(b, params[i].name);
    if (e != ASTOOLS_OK) return e;
    first = false;
  }
  return astools_buf_appends(b, "],\"additionalProperties\":false");
}

/* One property schema. Key order: "type"/"enum", "description", "default",
 * constraints (alphabetical), then items/properties/required/
 * additionalProperties/contentEncoding/format. */
static astools_err type_schema(astools_buf *b, const astools_type *t,
                               const char *desc, const xcdn_node_t *dflt,
                               int depth) {
  astools_err e;
  size_t i;
  if (depth > JSCM_MAX_DEPTH) return ASTOOLS_ERR_INVALID;
  /* "true" is the JSON Schema that accepts anything; only reachable on a
   * malformed in-memory type tree. */
  if (!t) return astools_buf_appends(b, "true");
  e = astools_buf_appendc(b, '{');
  if (e != ASTOOLS_OK) return e;
  if (t->kind == AT_ENUM) {
    e = astools_buf_appends(b, "\"enum\":[");
    if (e != ASTOOLS_OK) return e;
    for (i = 0; i < t->values_len; i++) {
      if (i > 0) {
        e = astools_buf_appendc(b, ',');
        if (e != ASTOOLS_OK) return e;
      }
      e = json_escape(b, t->values[i]);
      if (e != ASTOOLS_OK) return e;
    }
    e = astools_buf_appendc(b, ']');
    if (e != ASTOOLS_OK) return e;
  } else {
    e = astools_buf_printf(b, "\"type\":\"%s\"", json_type_name(t->kind));
    if (e != ASTOOLS_OK) return e;
  }
  e = emit_description(b, t, desc);
  if (e != ASTOOLS_OK) return e;
  if (dflt) {
    e = astools_buf_appends(b, ",\"default\":");
    if (e != ASTOOLS_OK) return e;
    e = default_json(b, dflt, 0);
    if (e != ASTOOLS_OK) return e;
  }
  switch (t->kind) {
  case AT_STRING:
  case AT_PATH:
    if (t->max_len >= 0) {
      e = astools_buf_printf(b, ",\"maxLength\":%" PRId64, t->max_len);
      if (e != ASTOOLS_OK) return e;
    }
    if (t->min_len >= 0) {
      e = astools_buf_printf(b, ",\"minLength\":%" PRId64, t->min_len);
      if (e != ASTOOLS_OK) return e;
    }
    break;
  case AT_INTEGER:
  case AT_NUMBER:
    if (t->has_max) {
      e = astools_buf_appends(b, ",\"maximum\":");
      if (e != ASTOOLS_OK) return e;
      e = json_number(b, t->max_num, t->kind == AT_INTEGER);
      if (e != ASTOOLS_OK) return e;
    }
    if (t->has_min) {
      e = astools_buf_appends(b, ",\"minimum\":");
      if (e != ASTOOLS_OK) return e;
      e = json_number(b, t->min_num, t->kind == AT_INTEGER);
      if (e != ASTOOLS_OK) return e;
    }
    break;
  case AT_ARRAY:
    if (t->max_items >= 0) {
      e = astools_buf_printf(b, ",\"maxItems\":%" PRId64, t->max_items);
      if (e != ASTOOLS_OK) return e;
    }
    if (t->min_items >= 0) {
      e = astools_buf_printf(b, ",\"minItems\":%" PRId64, t->min_items);
      if (e != ASTOOLS_OK) return e;
    }
    break;
  case AT_MAP:
    if (t->max_entries >= 0) {
      e = astools_buf_printf(b, ",\"maxProperties\":%" PRId64,
                             t->max_entries);
      if (e != ASTOOLS_OK) return e;
    }
    break;
  default:
    break;
  }
  switch (t->kind) {
  case AT_ARRAY:
    if (t->item) {
      e = astools_buf_appends(b, ",\"items\":");
      if (e != ASTOOLS_OK) return e;
      e = type_schema(b, t->item, NULL, NULL, depth + 1);
      if (e != ASTOOLS_OK) return e;
    }
    break;
  case AT_OBJECT:
    e = astools_buf_appendc(b, ',');
    if (e != ASTOOLS_OK) return e;
    e = object_body(b, t->fields, t->fields_len, depth + 1);
    if (e != ASTOOLS_OK) return e;
    break;
  case AT_MAP:
    if (t->value) {
      e = astools_buf_appends(b, ",\"additionalProperties\":");
      if (e != ASTOOLS_OK) return e;
      e = type_schema(b, t->value, NULL, NULL, depth + 1);
      if (e != ASTOOLS_OK) return e;
    }
    break;
  case AT_BYTES:
    e = astools_buf_appends(b, ",\"contentEncoding\":\"base64\"");
    if (e != ASTOOLS_OK) return e;
    break;
  case AT_DATETIME:
    e = astools_buf_appends(b, ",\"format\":\"date-time\"");
    if (e != ASTOOLS_OK) return e;
    break;
  case AT_DURATION:
    e = astools_buf_appends(b, ",\"format\":\"duration\"");
    if (e != ASTOOLS_OK) return e;
    break;
  case AT_UUID:
    e = astools_buf_appends(b, ",\"format\":\"uuid\"");
    if (e != ASTOOLS_OK) return e;
    break;
  default:
    break;
  }
  return astools_buf_appendc(b, '}');
}

/* ---- public (module) entry points ---------------------------------------- */

astools_err astools_jschema_input(const astools_cmd *cmd, char **out_json) {
  astools_buf b;
  astools_err e;
  if (!cmd || !out_json) return ASTOOLS_ERR_INVALID;
  *out_json = NULL;
  astools_buf_init(&b);
  e = astools_buf_appends(&b, "{\"type\":\"object\",");
  if (e == ASTOOLS_OK) e = object_body(&b, cmd->params, cmd->params_len, 0);
  if (e == ASTOOLS_OK) e = astools_buf_appendc(&b, '}');
  if (e != ASTOOLS_OK) {
    astools_buf_free(&b);
    return e;
  }
  *out_json = astools_buf_detach(&b);
  return *out_json != NULL ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
}

/* 1 when the manifest requests any fs write access. */
static bool perms_fs_write(const astools_perms *p) {
  size_t i;
  for (i = 0; i < p->fs_len; i++)
    if (p->fs[i].access & ASTOOLS_ACCESS_WRITE) return true;
  return false;
}

astools_err astools_mcp_description(const astools_tool *t,
                                    const astools_cmd *cmd, char **out_text) {
  astools_buf b;
  astools_err e;
  const astools_manifest *m;
  if (!t || !t->m || !cmd || !out_text) return ASTOOLS_ERR_INVALID;
  *out_text = NULL;
  m = t->m;
  astools_buf_init(&b);
  e = astools_buf_appends(&b, m->summary != NULL ? m->summary : "");
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, "\n\n");
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&b, cmd->summary != NULL ? cmd->summary : "");
  if (e == ASTOOLS_OK && !astools_str_blank(cmd->description)) {
    e = astools_buf_appends(&b, "\n\n");
    if (e == ASTOOLS_OK) e = astools_buf_appends(&b, cmd->description);
  }
  if (e == ASTOOLS_OK && cmd->examples_len > 0 && cmd->examples[0].call) {
    char *text = astools_xcdn_write_node(cmd->examples[0].call, false);
    if (!text) e = ASTOOLS_ERR_NOMEM;
    if (e == ASTOOLS_OK)
      e = astools_buf_printf(&b, "\n\nExample: %s.%s %s",
                             m->id != NULL ? m->id : "",
                             cmd->name != NULL ? cmd->name : "", text);
    free(text);
    if (e == ASTOOLS_OK && cmd->examples[0].result) {
      text = astools_xcdn_write_node(cmd->examples[0].result, false);
      if (!text) e = ASTOOLS_ERR_NOMEM;
      if (e == ASTOOLS_OK) e = astools_buf_printf(&b, " -> %s", text);
      free(text);
    }
  }
  if (e == ASTOOLS_OK && cmd->read_only)
    e = astools_buf_appends(&b, "\n\nRead-only.");
  if (e == ASTOOLS_OK && cmd->destructive && perms_fs_write(&m->perms))
    e = astools_buf_appends(&b, "\n\nWrites files under the granted paths.");
  if (e == ASTOOLS_OK && m->perms.proc)
    e = astools_buf_appends(&b,
                            "\n\nRequires the proc grant (off by default).");
  if (e == ASTOOLS_OK && m->perms.net)
    e = astools_buf_appends(&b, "\n\nRequires the net grant.");
  if (e == ASTOOLS_OK && cmd->deprecated)
    e = astools_buf_appends(&b, "\n\nDEPRECATED.");
  if (e == ASTOOLS_OK && cmd->long_running)
    e = astools_buf_appends(&b, "\n\nMay run long; set a timeout.");
  if (e != ASTOOLS_OK) {
    astools_buf_free(&b);
    return e;
  }
  *out_text = astools_buf_detach(&b);
  return *out_text != NULL ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
}
