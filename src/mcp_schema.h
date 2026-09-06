/* Deliberately bounded JSON Schema profile; unknown assertions fail admission. */
#ifndef ASTOOLS_MCP_SCHEMA_H
#define ASTOOLS_MCP_SCHEMA_H
#include "json.h"
#include <stdbool.h>
bool astools_mcp_schema_supported(const jx_value *schema);
/* Call only with a supported schema. Limits also reject the instance. */
bool astools_mcp_schema_validate(const jx_value *schema, const jx_value *value);
#endif
