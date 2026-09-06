/* This profile cannot silently erase a keyword from an operator-reviewed schema. */
#include "mcp_client.h"
#include "mcp_schema.h"
#include <math.h>

static bool named(const char *name, const char *const *names, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!strcmp(name, names[i])) return true;
  return false;
}
static bool strings(const jx_value *array) {
  if (jx_typeof(array) != JX_ARRAY || jx_array_len(array) > 256) return false;
  for (size_t i = 0; i < jx_array_len(array); i++) {
    const jx_value *v = jx_array_at(array, i);
    const char *s = jx_string_value(v);
    if (!s || strlen(s) != jx_string_length(v)) return false;
    for (size_t j = 0; j < i; j++)
      if (!strcmp(s, jx_string_value(jx_array_at(array, j)))) return false;
  }
  return true;
}
static bool supported(const jx_value *schema, unsigned depth, unsigned *left) {
  if (!schema || depth > 32 || !*left) return false;
  --*left;
  if (jx_typeof(schema) == JX_BOOL) return true;
  if (jx_typeof(schema) != JX_OBJECT) return false;
  static const char *const types[] = {"null",   "boolean", "number", "integer",
                                      "string", "array",   "object"};
  static const char *const counts[] = {"minLength", "maxLength",     "minItems",
                                       "maxItems",  "minProperties", "maxProperties"};
  static const char *const bounds[] = {"minimum", "maximum", "exclusiveMinimum",
                                       "exclusiveMaximum"};
  static const char *const text[] = {"title", "description", "$comment"};
  static const char *const flags[] = {"readOnly", "writeOnly", "deprecated"};
  for (size_t i = 0; i < jx_object_count(schema); i++) {
    const char *key = jx_object_key_at(schema, i);
    const jx_value *v = jx_object_value_at(schema, i);
    if (!strcmp(key, "$schema")) {
      const char *uri = astools_rpc_string(schema, key);
      if (!uri || strcmp(uri, "https://json-schema.org/draft/2020-12/schema")) return false;
    } else if (!strcmp(key, "type")) {
      const char *type = astools_rpc_string(schema, key);
      if (!type || !named(type, types, 7)) return false;
    } else if (!strcmp(key, "properties")) {
      if (jx_typeof(v) != JX_OBJECT || jx_object_count(v) > 256) return false;
      for (size_t j = 0; j < jx_object_count(v); j++)
        if (!supported(jx_object_value_at(v, j), depth + 1, left)) return false;
    } else if (!strcmp(key, "items") || !strcmp(key, "additionalProperties")) {
      if (!supported(v, depth + 1, left)) return false;
    } else if (!strcmp(key, "required")) {
      if (!strings(v)) return false;
    } else if (named(key, counts, 6)) {
      if (!jx_is_int(v) || jx_int_value(v) < 0 || jx_int_value(v) > 9007199254740992LL)
        return false;
    } else if (named(key, bounds, 4)) {
      /* Bounds inside the exact binary64 integer range keep mixed comparisons portable. */
      if (jx_typeof(v) != JX_NUMBER || fabs(jx_double_value(v)) > 9007199254740992.0 ||
          (jx_is_int(v) &&
           (jx_int_value(v) < -9007199254740992LL || jx_int_value(v) > 9007199254740992LL)))
        return false;
    } else if (!strcmp(key, "enum")) {
      if (jx_typeof(v) != JX_ARRAY || !jx_array_len(v) || jx_array_len(v) > 256) return false;
      for (size_t j = 0; j < jx_array_len(v); j++)
        for (size_t k = 0; k < j; k++)
          if (astools_mcp_schema_equal(jx_array_at(v, j), jx_array_at(v, k))) return false;
    } else if (named(key, text, 3)) {
      if (jx_typeof(v) != JX_STRING) return false;
    } else if (named(key, flags, 3)) {
      if (jx_typeof(v) != JX_BOOL) return false;
    } else if (!strcmp(key, "examples")) {
      if (jx_typeof(v) != JX_ARRAY) return false;
    } else if (strcmp(key, "default") && strcmp(key, "const"))
      return false;
  }
  return true;
}
bool astools_mcp_schema_supported(const jx_value *schema) {
  unsigned left = 1024;
  return supported(schema, 0, &left);
}
