/*
 * astools.h — public C API of libastools.
 *
 * astools (Asterism Tools) is a local, plug-and-play registry of system
 * tools for LLM agents: a tool is a directory dropped into a registry root,
 * described by one #astools_tool xCDN manifest that is simultaneously the
 * machine contract and the conversational interface.
 *
 * Contract notes:
 *  - All strings are UTF-8.
 *  - Every function returns astools_err except destructors.
 *  - Every out-allocation is released with astools_free / astools_result_free.
 *  - astools_open/astools_close are not thread-safe with respect to the same
 *    context; everything else is.
 *  - No global state: multiple contexts over different workspaces may
 *    coexist in one process.
 *
 * MIT License — see LICENSE.
 */

#ifndef ASTOOLS_H
#define ASTOOLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASTOOLS_VERSION "0.3.0"

typedef struct astools_ctx astools_ctx;

typedef enum {
  ASTOOLS_OK = 0,
  ASTOOLS_ERR_IO,
  ASTOOLS_ERR_PARSE,
  ASTOOLS_ERR_CONFIG,
  ASTOOLS_ERR_NOT_FOUND,
  ASTOOLS_ERR_INVALID,
  ASTOOLS_ERR_DENIED,
  ASTOOLS_ERR_TIMEOUT,
  ASTOOLS_ERR_CANCELLED,
  ASTOOLS_ERR_TOOL,
  ASTOOLS_ERR_PROTOCOL,
  ASTOOLS_ERR_UNSUPPORTED,
  ASTOOLS_ERR_BUSY,
  ASTOOLS_ERR_NOMEM
} astools_err;

/* Stable identifier of an error code ("ASTOOLS_ERR_DENIED", ...). */
const char *astools_err_name(astools_err e);

typedef struct {
  /* NULL-terminated array of registry roots; NULL = use configuration.
   * Roots given here are trust level "standard". */
  const char *const *registry_paths;
  const char *config_path;    /* optional config.xcdn; NULL = defaults   */
  const char *workspace_root; /* overrides workspace.root; NULL = config */
} astools_open_params;

astools_err astools_open(const astools_open_params *p, astools_ctx **out);
void        astools_close(astools_ctx *c); /* joins the supervisor */

/* Last engine-level error message for this context (UTF-8, ctx-owned;
 * valid until the next failing call on the same thread's context). */
const char *astools_last_error(const astools_ctx *c);

/* ---- registry ------------------------------------------------------ */

astools_err astools_registry_refresh(astools_ctx *c);

/* Resolved tool ids, one per id (highest resolvable version). Free the
 * array and each element with astools_free. */
astools_err astools_tool_list(astools_ctx *c, char ***out_ids, size_t *n);

/* Manifest of the resolved tool ("fs" or "fs@1.2.0"), re-serialized as
 * canonical xCDN text. */
astools_err astools_tool_manifest(astools_ctx *c, const char *ref,
                                  char **out_xcdn);

astools_err astools_tool_enable(astools_ctx *c, const char *ref, int on);

/* Recompute and record lockfile hashes for the resolved tool. */
astools_err astools_tool_approve(astools_ctx *c, const char *ref);

/* ---- conversational interface -------------------------------------- */

typedef enum {
  ASTOOLS_CATALOG_INDEX = 0,
  ASTOOLS_CATALOG_SUMMARY,
  ASTOOLS_CATALOG_FULL
} astools_catalog_level;

/* Deterministic prompt-facing rendering of the enabled registry under a
 * character budget (0 = unlimited). */
astools_err astools_catalog(astools_ctx *c, astools_catalog_level level,
                            size_t char_budget, char **out_text);

/* llama.cpp GBNF grammar whose language is exactly the valid call lines
 * of the current registry. */
astools_err astools_grammar_export(astools_ctx *c, char **out_gbnf);

/* First call line in model output; ASTOOLS_ERR_NOT_FOUND if none. */
astools_err astools_call_parse(astools_ctx *c, const char *model_output,
                               char **out_ref, char **out_command,
                               char **out_args_xcdn);

struct astools_result_s;

/* Render one RESULT/ERROR line for handing an outcome back to the model. */
astools_err astools_call_format(astools_ctx *c, const char *ref,
                                const char *command,
                                const struct astools_result_s *r,
                                char **out_line);

/* ---- invocation ----------------------------------------------------- */

typedef struct astools_result_s {
  int      ok;            /* tool-level success                       */
  char    *result_xcdn;   /* xCDN value; NULL when !ok                */
  char    *error_code;    /* e.g. "fs/not-found"; NULL when ok        */
  char    *error_message; /* NULL when ok                             */
  int      exit_code;     /* executable tools; 0 otherwise            */
  uint64_t duration_ms;
} astools_result;

/* Validate args against the command schema without dispatching. */
astools_err astools_validate_args(astools_ctx *c, const char *ref,
                                  const char *command,
                                  const char *args_xcdn);

/* Blocking invocation; deadline_ms = 0 -> command/config default. */
astools_err astools_invoke(astools_ctx *c, const char *ref,
                           const char *command, const char *args_xcdn,
                           uint32_t deadline_ms, astools_result *out);

typedef struct astools_task astools_task;

astools_err astools_invoke_async(astools_ctx *c, const char *ref,
                                 const char *command, const char *args_xcdn,
                                 uint32_t deadline_ms, astools_task **out);
/* ASTOOLS_ERR_BUSY: not done within timeout_ms. On completion fills *out
 * exactly once; later calls return the stored outcome. */
astools_err astools_task_wait(astools_task *t, uint32_t timeout_ms,
                              astools_result *out);
astools_err astools_task_cancel(astools_task *t);
void        astools_task_free(astools_task *t);

/* ---- sandbox / observability --------------------------------- */

typedef struct {
  int fs_confinement; /* 1 = kernel-enforced at the queried level */
  int net_deny;
  int syscall_filter;
  int memory_cap;
  int cpu_cap;
  int process_cap;
  int privilege_drop;
} astools_sandbox_caps;

astools_err astools_get_sandbox_caps(astools_ctx *c, int strict,
                                     astools_sandbox_caps *out);

typedef struct {
  size_t tools_total, tools_enabled, tools_unavailable;
  size_t invocations, ok, failed, denied, timeouts, cancelled;
  int64_t last_refresh_unix;    /* 0 = never */
  int64_t last_invocation_unix; /* 0 = never */
} astools_stats;

astools_err astools_get_stats(astools_ctx *c, astools_stats *out);

enum {
  ASTOOLS_LOG_ERROR = 0,
  ASTOOLS_LOG_WARN = 1,
  ASTOOLS_LOG_INFO = 2,
  ASTOOLS_LOG_DEBUG = 3
};

typedef void (*astools_log_fn)(int level, const char *msg, void *ud);
/* Install/replace the host log callback (NULL disables it). Independent of
 * the file sink configured via logging.path. */
void astools_set_logger(astools_ctx *c, astools_log_fn fn, void *ud);

/* ASTOOLS_NO_THREADS builds: run due work (registry poll, reaping). */
astools_err astools_tick(astools_ctx *c);

void astools_result_free(astools_result *r);
void astools_free(void *p); /* strings/arrays returned by the API */

#ifdef __cplusplus
}
#endif

#endif /* ASTOOLS_H */
