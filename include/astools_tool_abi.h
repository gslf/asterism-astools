/*
 * astools_tool_abi.h — C ABI for in-process tools of kind "library".
 *
 * A library tool is loaded with dlopen/LoadLibrary from a "full"-trust
 * registry root, only when sandbox.allow_library is true. It runs with the
 * host's privileges and cannot be sandboxed; deadlines are advisory. The
 * wire types are identical to the executable protocol, so a tool can
 * be developed as an executable and promoted to a library without changing
 * its protocol code.
 *
 * MIT License — see LICENSE.
 */

#ifndef ASTOOLS_TOOL_ABI_H
#define ASTOOLS_TOOL_ABI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASTOOLS_TOOL_ABI 1

typedef struct {
  uint32_t abi_version; /* must equal ASTOOLS_TOOL_ABI */

  /* Called once after load with the tool's own manifest as xCDN text.
   * Return 0 on success; nonzero fails every invocation of the tool. */
  int (*init)(const char *manifest_xcdn);

  void (*shutdown)(void);

  /* input: one #tool_request as xCDN text;
   * output: one #tool_response as xCDN text, released by the runtime
   * through free_result. Must be thread-safe. */
  char *(*invoke)(const char *request_xcdn);

  void (*free_result)(char *response_xcdn);
} astools_tool_vtable;

/* The single exported symbol of a library tool. */
const astools_tool_vtable *astools_tool_entry(void);

typedef const astools_tool_vtable *(*astools_tool_entry_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* ASTOOLS_TOOL_ABI_H */
