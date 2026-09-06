/* Tiny independent native tool for identity and request-correlation tests. */
#include "astools_tool_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int init(const char *manifest) { return manifest ? 0 : 1; }
static void shutdown_tool(void) {}
static char *invoke(const char *request) {
  const char *id = strstr(request, "invocation_id");
  id = id ? strstr(id, "u\"") : NULL;
  if (!id || strlen(id) < 39) return NULL;
  id += 2;
  if (strstr(request, "_wrong_request_")) id = "00000000-0000-4000-8000-000000000000";
  char *out = malloc(256);
  if (out)
    snprintf(out, 256,
             "#tool_response {protocol:1,invocation_id:u\"%.36s\",ok:true,result:{loaded:true}}",
             id);
  return out;
}
static void free_result(char *text) { free(text); }
const astools_tool_vtable *astools_tool_entry(void) {
  static const astools_tool_vtable v = {ASTOOLS_TOOL_ABI, init, shutdown_tool, invoke, free_result};
  return &v;
}
