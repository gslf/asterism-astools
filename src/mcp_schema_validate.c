/* Validate arguments before dispatch and structured results before accepting success. */
#include "mcp_client.h"
#include "mcp_schema.h"
#include <math.h>

static bool count_ok(const jx_value *schema, const char *min, const char *max, size_t n) {
  const jx_value *lo = jx_object_get(schema, min), *hi = jx_object_get(schema, max);
  return (!lo || (uint64_t)n >= (uint64_t)jx_int_value(lo)) &&
         (!hi || (uint64_t)n <= (uint64_t)jx_int_value(hi));
}
static bool number_ok(const jx_value *schema, const jx_value *value) {
  static const char *const keys[] = {"minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum"};
  for (size_t i = 0; i < 4; i++) {
    const jx_value *bound = jx_object_get(schema, keys[i]);
    if (!bound) continue;
    int order;
    if (jx_is_int(value) && jx_is_int(bound)) {
      long long a = jx_int_value(value), b = jx_int_value(bound);
      order = (a > b) - (a < b);
    } else if (jx_is_int(value) && jx_int_value(value) > 9007199254740992LL)
      order = 1;
    else if (jx_is_int(value) && jx_int_value(value) < -9007199254740992LL)
      order = -1;
    else {
      double a = jx_double_value(value), b = jx_double_value(bound);
      order = (a > b) - (a < b);
    }
    if ((i == 0 && order < 0) || (i == 1 && order > 0) || (i == 2 && order <= 0) ||
        (i == 3 && order >= 0))
      return false;
  }
  return true;
}
static bool validate(const jx_value *schema, const jx_value *value, unsigned depth,
                     unsigned *left) {
  if (!value || !*left || depth > 64) return false;
  --*left;
  if (!schema) return true;
  if (jx_typeof(schema) == JX_BOOL) return jx_bool_value(schema) != 0;
  static const char *const types[] = {"null", "boolean", "number", "string", "array", "object"};
  const char *type = astools_rpc_string(schema, "type");
  if (type && strcmp(type, types[jx_typeof(value)])) {
    double n = jx_double_value(value);
    if (strcmp(type, "integer") || jx_typeof(value) != JX_NUMBER || floor(n) != n) return false;
  }
  const jx_value *fixed = jx_object_get(schema, "const"), *choices = jx_object_get(schema, "enum");
  if (fixed && !astools_mcp_schema_equal(fixed, value)) return false;
  if (choices) {
    bool found = false;
    for (size_t i = 0; i < jx_array_len(choices); i++)
      if (astools_mcp_schema_equal(jx_array_at(choices, i), value)) {
        found = true;
        break;
      }
    if (!found) return false;
  }
  switch (jx_typeof(value)) {
  case JX_NUMBER:
    return number_ok(schema, value);
  case JX_STRING: {
    size_t n = 0;
    const unsigned char *bytes = (const unsigned char *)jx_string_value(value);
    for (size_t i = 0; i < jx_string_length(value); i++)
      if ((bytes[i] & 0xc0) != 0x80) n++;
    return count_ok(schema, "minLength", "maxLength", n);
  }
  case JX_ARRAY:
    if (!count_ok(schema, "minItems", "maxItems", jx_array_len(value))) return false;
    for (size_t i = 0; i < jx_array_len(value); i++)
      if (!validate(jx_object_get(schema, "items"), jx_array_at(value, i), depth + 1, left))
        return false;
    return true;
  case JX_OBJECT: {
    if (!count_ok(schema, "minProperties", "maxProperties", jx_object_count(value))) return false;
    const jx_value *required = jx_object_get(schema, "required"),
                   *props = jx_object_get(schema, "properties");
    for (size_t i = 0; i < jx_array_len(required); i++)
      if (!jx_object_get(value, jx_string_value(jx_array_at(required, i)))) return false;
    for (size_t i = 0; i < jx_object_count(value); i++) {
      const jx_value *child = jx_object_get(props, jx_object_key_at(value, i));
      if (!child) child = jx_object_get(schema, "additionalProperties");
      if (!validate(child, jx_object_value_at(value, i), depth + 1, left)) return false;
    }
    return true;
  }
  default:
    return true;
  }
}
bool astools_mcp_schema_validate(const jx_value *schema, const jx_value *value) {
  unsigned left = 32768;
  return validate(schema, value, 0, &left);
}
