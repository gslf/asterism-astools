/*
 * sdk.h — shared plumbing of the astools-std multi-tool binary.
 *
 * astools-std is one busybox-style executable dispatched on `--tool <id>`.
 * A tool invocation reads exactly one #tool_request from stdin, does its
 * work, and writes exactly one #tool_response to stdout. This SDK
 * owns the wire protocol so tool_*.c files contain only tool logic.
 *
 * Links xcdn only — never libastools (tools are ordinary packages).
 *
 * Ownership: astd_ok takes ownership of the result value; astd_fail owns
 * nothing. Both write the response, flush stdout, and are called exactly
 * once per process.
 */

#ifndef ASTOOLS_STD_SDK_H
#define ASTOOLS_STD_SDK_H

#include <stdint.h>

#include "xcdn.h"

/* One parsed #tool_request. Borrowed pointers live in doc. */
typedef struct {
  xcdn_document_t *doc;
  const char *invocation_id; /* uuid string */
  const char *tool;
  const char *version;
  const char *command;
  const xcdn_value_t *args;   /* object value; never NULL after read */
  const xcdn_value_t *grants; /* object value; may be NULL (informative) */
  const char *workspace;      /* absolute; may be NULL */
  const char *scratch;        /* absolute; may be NULL */
  int64_t max_output_bytes;   /* 0 = unlimited */
} astd_req;

/* Read all of stdin, parse the request. Returns 0 on success; on failure
 * prints a protocol-error #tool_response (best effort) and returns
 * nonzero (caller exits nonzero). */
int  astd_req_read(astd_req *r);
void astd_req_free(astd_req *r);

/* ---- argument accessors -------------------------------------------------
 * The runtime already validated and canonicalized args: types
 * are exact, defaults injected, paths absolute. Accessors therefore trust
 * the shape; dflt covers only genuinely absent optional params (possible
 * when a host runs with validation off). */
const xcdn_value_t *astd_arg(const astd_req *r, const char *name);
const char *astd_arg_str(const astd_req *r, const char *name,
                         const char *dflt);
int64_t     astd_arg_int(const astd_req *r, const char *name, int64_t dflt);
int         astd_arg_bool(const astd_req *r, const char *name, int dflt);
double      astd_arg_num(const astd_req *r, const char *name, double dflt);

/* ---- responses ---------------------------------------------------------- */

/* Emit #tool_response{ok:true, result}; takes ownership of result. */
void astd_ok(const astd_req *r, xcdn_value_t *result);
/* Emit #tool_response{ok:false, error:{code,message,retriable:false}}. */
void astd_fail(const astd_req *r, const char *code, const char *fmt, ...);

/* ---- result-building helpers (thin xcdn sugar) --------------------------- */

/* xcdn_object_set with constructed nodes; each returns 0 on success. */
int astd_set_str(xcdn_value_t *obj, const char *k, const char *v);
int astd_set_int(xcdn_value_t *obj, const char *k, int64_t v);
int astd_set_bool(xcdn_value_t *obj, const char *k, int v);
int astd_set_val(xcdn_value_t *obj, const char *k, xcdn_value_t *v /*owned*/);
int astd_arr_push_val(xcdn_value_t *arr, xcdn_value_t *v /*owned*/);

/* Format unix seconds as RFC 3339 UTC into out[32]. */
void astd_rfc3339(int64_t unix_s, char out[32]);

/* Base64 (RFC 4648, no wrap). Encode returns malloc'd NUL-terminated;
 * decode returns 0 on success with a malloc'd buffer. */
char *astd_base64_encode(const uint8_t *data, size_t len);
int   astd_base64_decode(const char *s, uint8_t **out, size_t *out_len);

/* Shell-free glob match (fs.list / grep): '*' any run (not '/'),
 * '?' any char (not '/'), '['…']' classes; matched against the final path
 * component unless the pattern contains '/'. Returns 1 on match. */
int astd_glob_match(const char *pattern, const char *path);

/* 1 when buf[0..n) looks binary (NUL byte in the first 4 KiB). */
int astd_looks_binary(const char *buf, size_t n);

/* ---- child process runner ----------------------------------------------
 * Shared by proc.run and closed-argv semantic wrappers.  This is an
 * implementation detail of astools-std, not part of libastools. */
typedef struct {
  int exit_code;
  int timed_out;
  char *out;
  size_t out_n;
  int out_trunc;
  char *err;
  size_t err_n;
  int err_trunc;
} astd_run_res;

int astd_run_capture(char *const *argv, char *const *envp, const char *cwd,
                     const char *input, size_t input_n, size_t out_cap,
                     size_t err_cap, int64_t timeout_ms, astd_run_res *rr,
                     char *emsg, size_t emsg_sz);

/* ---- tool entry points (tool_*.c) ---------------------------------------
 * Called after astd_req_read; each returns the process exit code (0 unless
 * the response could not be written). */
int astd_tool_fs(astd_req *r);
int astd_tool_grep(astd_req *r);
int astd_tool_edit(astd_req *r);
int astd_tool_sys(astd_req *r);
int astd_tool_env(astd_req *r);
int astd_tool_proc(astd_req *r);
int astd_tool_git(astd_req *r);
int astd_tool_code(astd_req *r);
int astd_tool_project(astd_req *r);
#ifdef ASTOOLS_BUILD_NET
int astd_tool_net(astd_req *r);
#endif

/* ---- regex.c — in-house linear-time ERE subset -------------- */

typedef struct astd_regex astd_regex;

/* Compile; NULL on error with a static-lifetime message in *err
 * pinpointing the offending construct. case_sensitive=0 folds ASCII. */
astd_regex *astd_regex_compile(const char *pattern, int case_sensitive,
                               const char **err);
void astd_regex_free(astd_regex *re);

/* Search line[0..len) for the leftmost match. Returns 1 and fills
 * *start / *end (byte offsets, end exclusive) on match. Anchors ^/$ match
 * line boundaries (the buffer is one line). Guaranteed O(pattern×len). */
int astd_regex_search(const astd_regex *re, const char *line, size_t len,
                      size_t *start, size_t *end);

#endif /* ASTOOLS_STD_SDK_H */
