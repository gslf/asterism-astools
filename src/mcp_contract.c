/* Schema metadata can bind a reviewed contract, never grant a capability. */
#include "mcp_client.h"
#include "mcp_schema.h"
static const char *string(const xcdn_node_t *node, const char *key) {
  const xcdn_node_t *v = node && node->value ? xcdn_object_get(node->value, key) : NULL;
  return v && v->value && v->value->type == XCDN_VAL_STRING ? v->value->data.string : NULL;
}
static bool schema(const char *text, bool input) {
  jx_value *value = NULL;
  if (!text || strlen(text) > 65536 || jx_parse(text, strlen(text), &value)) return false;
  const char *type = astools_rpc_string(value, "type");
  bool valid = astools_mcp_schema_supported(value) && (!input || (type && !strcmp(type, "object")));
  jx_free(value);
  return valid;
}
bool astools_mcp_command_parse(const astools_manifest *m, astools_cmd *cmd,
                               const xcdn_node_t *binding) {
  if (m->protocol != ASTOOLS_PROTOCOL_MCP) return !binding;
  if (!binding || !binding->value || binding->value->type != XCDN_VAL_OBJECT || cmd->idempotent)
    return false;
  const char *name = string(binding, "name"), *input = string(binding, "input_schema"),
             *output = string(binding, "output_schema");
  if (!name || !*name || strlen(name) > 128 ||
      strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
          strlen(name) ||
      !schema(input, true) ||
      (xcdn_object_get(binding->value, "output_schema") && !schema(output, false)))
    return false;
  if (binding->value->data.object.len != (output ? 3u : 2u)) return false;
  cmd->mcp_name = astools_strdup(name);
  cmd->mcp_input_schema = astools_strdup(input);
  if (output) cmd->mcp_output_schema = astools_strdup(output);
  return cmd->mcp_name && cmd->mcp_input_schema && (!output || cmd->mcp_output_schema);
}

/* Object order is immaterial; arrays and every schema keyword retain meaning. */
bool astools_mcp_schema_equal(const jx_value *a, const jx_value *b) {
  if (!a || !b) return a == b;
  if (jx_typeof(a) != jx_typeof(b)) return false;
  switch (jx_typeof(a)) {
  case JX_NULL:
    return true;
  case JX_BOOL:
    return jx_bool_value(a) == jx_bool_value(b);
  case JX_NUMBER:
    if (jx_is_int(a) && jx_is_int(b)) return jx_int_value(a) == jx_int_value(b);
    if (jx_is_int(a) != jx_is_int(b)) {
      long long n = jx_int_value(jx_is_int(a) ? a : b);
      if (n < -9007199254740992LL || n > 9007199254740992LL) return false;
    }
    return jx_double_value(a) == jx_double_value(b);
  case JX_STRING:
    return jx_string_length(a) == jx_string_length(b) &&
           !memcmp(jx_string_value(a), jx_string_value(b), jx_string_length(a));
  case JX_ARRAY:
    if (jx_array_len(a) != jx_array_len(b)) return false;
    for (size_t i = 0; i < jx_array_len(a); i++)
      if (!astools_mcp_schema_equal(jx_array_at(a, i), jx_array_at(b, i))) return false;
    return true;
  case JX_OBJECT:
    if (jx_object_count(a) != jx_object_count(b)) return false;
    for (size_t i = 0; i < jx_object_count(a); i++)
      if (!astools_mcp_schema_equal(jx_object_value_at(a, i),
                                    jx_object_get(b, jx_object_key_at(a, i))))
        return false;
    return true;
  }
  return false;
}

/* JSON-only arguments: dates, tags, NaN and opaque xCDN extensions cannot leak
 * through a lossy serializer. Policy has already canonicalized path strings. */
static jx_value *arguments(const xcdn_node_t *node, unsigned depth) {
  if (!node || !node->value || node->tags_len || depth > 64) return NULL;
  const xcdn_value_t *v = node->value;
  switch (v->type) {
  case XCDN_VAL_NULL:
    return jx_null();
  case XCDN_VAL_BOOL:
    return jx_bool(v->data.boolean);
  case XCDN_VAL_INT:
    return jx_int(v->data.integer);
  case XCDN_VAL_FLOAT:
    return jx_double(v->data.floating);
  case XCDN_VAL_STRING:
    return jx_string(v->data.string);
  case XCDN_VAL_ARRAY: {
    jx_value *out = jx_array();
    for (size_t i = 0; out && i < xcdn_array_len(v); i++)
      if (jx_array_push(out, arguments(xcdn_array_get(v, i), depth + 1))) {
        jx_free(out);
        return NULL;
      }
    return out;
  }
  case XCDN_VAL_OBJECT: {
    jx_value *out = jx_object();
    for (size_t i = 0; out && i < v->data.object.len; i++)
      if (jx_object_set(out, v->data.object.entries[i].key,
                        arguments(v->data.object.entries[i].node, depth + 1))) {
        jx_free(out);
        return NULL;
      }
    return out;
  }
  default:
    return NULL;
  }
}
jx_value *astools_mcp_arguments(const xcdn_node_t *node) {
  return arguments(node, 0);
}

/* Shared admission makes selection/batch validation as strict as actual dispatch. */
astools_err astools_mcp_validate(astools_ctx *c, const astools_cmd *cmd, const xcdn_node_t *args) {
  if (!cmd->mcp_input_schema) return ASTOOLS_OK;
  jx_value *schema = NULL, *value = astools_mcp_arguments(args);
  bool valid = value && !jx_parse(cmd->mcp_input_schema,strlen(cmd->mcp_input_schema),&schema) &&
               astools_mcp_schema_validate(schema,value);
  jx_free(schema); jx_free(value);
  return valid ? ASTOOLS_OK : astools_seterr(c,ASTOOLS_ERR_INVALID,
      "Arguments violate the reviewed MCP input schema or its JSON/resource limits");
}
