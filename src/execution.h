/* Private runtime state, shared only by the execution modules. */
#ifndef ASTOOLS_EXECUTION_H
#define ASTOOLS_EXECUTION_H
#include "astools_internal.h"
#include "astools_tool_abi.h"
#include <stdlib.h>
#include <string.h>
#define ASTOOLS_INSTANCE_LIMIT 64
#define PP_KIND_PROC 0
#define PP_KIND_LIB 1

/* One member of a keyed persistent pool (guarded by c->pp_mu). The busy
 * count pins it against the idle reaper; process streams have one borrower.
 * io_mu protects library initialization only. A pool grows to the manifest's parallel limit. */
struct astools_pproc {
  int kind;                    /* PP_KIND_* */
  char key[65];                /* immutable package and path identity */
  astools_tool *tool;          /* retained while this instance exists */
  astools_sandbox_setup setup; /* owned until the child stops */
  int busy;
  os_mutex io_mu;
  struct astools_pproc *next;
  /* PP_KIND_PROC */
  os_proc proc;
  bool alive, hello_done;
  int64_t last_used_mono;
  int64_t backoff_ms, next_restart_mono;
  int64_t idle_timeout_ms;
  astools_buf pending; /* unconsumed stdout bytes between reads */
  /* PP_KIND_LIB */
  os_dylib lib;
  const astools_tool_vtable *vt;
  bool lib_loaded, lib_init_ok, lib_failed;
};

enum { PP_WHY_NONE = 0, PP_WHY_EOF, PP_WHY_OVERFLOW };
void astools_exec_result_clear(astools_result *r);
void astools_exec_result_set(astools_result *r, const char *code, const char *fmt, ...);
void astools_exec_sleep(int64_t ms);
void astools_pp_node_free(astools_pproc *p);
astools_err astools_pp_acquire(astools_ctx *c, int kind, astools_tool *t, int64_t deadline,
                               astools_task *cancel, astools_pproc **out);
void astools_pp_release(astools_ctx *c, astools_pproc *p);
void astools_pp_stop(astools_ctx *c, astools_pproc *p, bool graceful);
void astools_pp_backoff(astools_ctx *c, astools_pproc *p);
astools_err astools_pp_read_line(astools_ctx *c, astools_pproc *p, int64_t deadline,
                                 astools_task *cancel, char **out, size_t *len, int *why);
astools_err astools_pp_write_all(astools_ctx *c, astools_pproc *p, const char *text,
                                 int64_t deadline, astools_task *cancel, int *why);
astools_err astools_pp_ensure_alive(astools_ctx *c, const astools_tool *t,
                                    const astools_effective *eff, astools_pproc *p,
                                    int64_t deadline, astools_task *cancel, astools_result *r);
astools_err astools_pp_validate(astools_ctx *c, const astools_tool *t);
#endif
