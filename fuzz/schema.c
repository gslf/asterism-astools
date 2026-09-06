/* Supported contracts must retain their admission decision after cloning. */
#include "mcp_client.h"
#include "mcp_schema.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  jx_value *input = NULL;
  if (size > 16384 || jx_parse((const char *)data, size, &input))
    return 0;
  const jx_value *schema = jx_array_at(input, 0), *instance = jx_array_at(input, 1);
  if (!schema || !instance)
    goto done;
  jx_value *copy = jx_clone(schema), *value = jx_clone(instance);
  if (!copy || !value)
    goto release;
  if (!astools_mcp_schema_equal(schema, copy) || !astools_mcp_schema_equal(instance, value) ||
      astools_mcp_schema_equal(schema, instance) != astools_mcp_schema_equal(instance, schema))
    abort();
  bool accepted = astools_mcp_schema_supported(schema);
  if (accepted != astools_mcp_schema_supported(copy))
    abort();
  if (accepted &&
      astools_mcp_schema_validate(schema, instance) != astools_mcp_schema_validate(copy, value))
    abort();
release:
  jx_free(copy);
  jx_free(value);
done:
  jx_free(input);
  return 0;
}
