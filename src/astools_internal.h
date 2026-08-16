/*
 * astools_internal.h — internal contracts of libastools.
 *
 * Module map (one .c per row; a module implements exactly the functions
 * declared in its section plus static helpers):
 *
 *   util.c          buffers, strings, fnv, base64, slug checks
 *   time.c          RFC 3339 + ISO 8601 durations, injectable clock
 *   uuid.c          UUID v4 generation / validation
 *   sha256.c        SHA-256 (streaming + one-shot + file)
 *   log.c           leveled rotating file log + host callback + audit
 *   types.c         #type parsing, strict validation, defaults
 *   manifest.c      #astools_tool parsing, schema + lint
 *   semver.c        SemVer 2.0.0 parse/compare
 *   registry.c      roots scan, table publish, resolution
 *   lockfile.c      astools.lock.xcdn load/check/approve
 *   config.c        defaults + config.xcdn
 *   path.c          canonicalization, prefix containment
 *   policy.c        grants intersection + pre-flight
 *   sandbox_posix.c env scrub, limits, seatbelt profile, jail
 *   proto.c         #tool_request/#tool_response build/parse
 *   invoke.c        the pipeline
 *   worker.c        oneshot exec, async tasks, persistent table, poll
 *   catalog.c       budgeted catalog rendering
 *   gbnf.c          GBNF export
 *   callline.c      CALL/RESULT/ERROR lines
 *   jscm.c          JSON Schema + MCP description strings
 *   api.c           public API funnel, open/close wiring
 *   os_*.c          platform shim (os.h)
 *
 * Locking protocol: c->lock (rwlock) guards the registry table,
 * enable flags, lock-state and stats. Invocations do NOT hold c->lock
 * while a tool runs — they pin the resolved astools_tool by refcount
 * (astools_tool_ref/unref). c->slot_mu guards slot admission. c->pp_mu
 * guards the persistent-process table. c->log_mu guards the file sink;
 * c->err_mu guards err_buf. Lock order: lock -> pp_mu -> log_mu. Never
 * take c->lock while holding any other astools lock.
 */

#ifndef ASTOOLS_INTERNAL_H
#define ASTOOLS_INTERNAL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "astools.h"
#include "os.h"
#include "xcdn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct astools_tool astools_tool;
typedef struct astools_cmd astools_cmd;

/* ═══════════════════════ util.c ═══════════════════════ */

typedef struct {
  char *data; /* always NUL-terminated after init/append */
  size_t len, cap;
} astools_buf;

void        astools_buf_init(astools_buf *b);
void        astools_buf_free(astools_buf *b);
astools_err astools_buf_append(astools_buf *b, const char *data, size_t len);
astools_err astools_buf_appends(astools_buf *b, const char *s);
astools_err astools_buf_appendc(astools_buf *b, char ch);
astools_err astools_buf_printf(astools_buf *b, const char *fmt, ...);
char       *astools_buf_detach(astools_buf *b);

char *astools_strdup(const char *s); /* NULL-safe */
char *astools_strndup(const char *s, size_t n);
char *astools_str_trim(char *s);
bool  astools_str_blank(const char *s);

uint64_t astools_fnv1a64(const void *data, size_t len);

char *astools_base64_encode(const uint8_t *data, size_t len);
bool  astools_base64_decode(const char *s, uint8_t **out, size_t *out_len);

/* Slug: [a-z0-9][a-z0-9-]{0,63}. Manifest ids additionally cap at 32. */
bool astools_slug_valid(const char *s);

int astools_token_heuristic(const char *text);

/* ═══════════════════════ time.c ═══════════════════════ */

typedef int64_t astools_time; /* unix seconds, UTC */

typedef struct {
  void *ud;
  astools_time (*now)(void *ud);
} astools_clock;

astools_time  astools_clock_now(const astools_clock *clk);
astools_clock astools_clock_system(void);

bool astools_time_parse_rfc3339(const char *s, astools_time *out);
void astools_time_format_rfc3339(astools_time t, char out[32]);
/* ISO 8601 duration into seconds; months/years rejected. */
bool astools_duration_parse(const char *s, int64_t *out_seconds);

/* ═══════════════════════ uuid.c ═══════════════════════ */

void astools_uuid_v4(char out[37]);
bool astools_uuid_valid(const char *s);
bool astools_uuid_to_bytes(const char *s, uint8_t out[16]);
void astools_uuid_from_bytes(const uint8_t in[16], char out[37]);

/* ═══════════════════════ sha256.c ═══════════════════════ */

typedef struct {
  uint32_t state[8];
  uint64_t bitlen;
  uint8_t buffer[64];
  size_t buflen;
} astools_sha256_ctx;

void astools_sha256_init(astools_sha256_ctx *ctx);
void astools_sha256_update(astools_sha256_ctx *ctx, const void *data,
                           size_t len);
void astools_sha256_final(astools_sha256_ctx *ctx, uint8_t out[32]);
void astools_sha256(const void *data, size_t len, uint8_t out[32]);
astools_err astools_sha256_file(const char *path, uint8_t out[32]);

/* ═══════════════════════ types.c ═══════════════════════ */

typedef enum {
  AT_STRING = 0,
  AT_INTEGER,
  AT_NUMBER,
  AT_BOOLEAN,
  AT_BYTES,
  AT_DATETIME,
  AT_DURATION,
  AT_UUID,
  AT_PATH,
  AT_ENUM,
  AT_ARRAY,
  AT_MAP,
  AT_OBJECT
} astools_tkind;

/* Access is a bitmask so grant coverage is a subset test. */
#define ASTOOLS_ACCESS_READ 1
#define ASTOOLS_ACCESS_WRITE 2
#define ASTOOLS_ACCESS_RW (ASTOOLS_ACCESS_READ | ASTOOLS_ACCESS_WRITE)

typedef struct astools_type astools_type;
typedef struct astools_param astools_param;

struct astools_type {
  astools_tkind kind;
  /* string/bytes: -1 = unset */
  int64_t min_len, max_len;
  /* integer/number */
  bool has_min, has_max;
  double min_num, max_num;
  /* path */
  int access; /* ASTOOLS_ACCESS_* */
  bool must_exist;
  /* enum */
  char **values;
  size_t values_len;
  /* array */
  astools_type *item;
  int64_t min_items, max_items; /* -1 = unset */
  /* map */
  astools_type *value;
  int64_t max_entries; /* -1 = unset */
  /* object: closed field list */
  astools_param *fields;
  size_t fields_len;
};

struct astools_param {
  char *name;
  astools_type *type;
  bool required;
  xcdn_node_t *dflt; /* owned deep copy; NULL = none. Optional only. */
  char *description;  /* may be NULL */
};

/* Parse a #type node. On failure returns NULL and writes a message into
 * err (truncated). */
astools_type *astools_type_parse(const xcdn_node_t *node, char *err,
                                 size_t err_cap);
void astools_type_free(astools_type *t);
void astools_param_free_fields(astools_param *p); /* frees members, not p */

/* Strict recursive check of value against t (no coercion). Path
 * params check as strings here; resolution happens in policy. On mismatch
 * returns ASTOOLS_ERR_INVALID and writes "param x: expected integer, got
 * string"-style text into err. ctxname is the parameter path for
 * messages. */
astools_err astools_type_check(const astools_type *t, const xcdn_node_t *v,
                               const char *ctxname, char *err,
                               size_t err_cap);

/* Validate an args OBJECT against cmd->params: strict types, required
 * present, unknown keys rejected, defaults for absent optionals injected
 * into args (mutated). */
astools_err astools_args_validate(const astools_cmd *cmd, xcdn_node_t *args,
                                  char *err, size_t err_cap);

/* ═══════════════════════ manifest.c ═══════════════════════ */

#define ASTOOLS_KIND_EXECUTABLE 0
#define ASTOOLS_KIND_LIBRARY 1
#define ASTOOLS_MODE_ONESHOT 0
#define ASTOOLS_MODE_PERSISTENT 1

typedef struct {
  char *path;  /* after ${workspace} expansion; may be relative (rare) */
  int access;  /* ASTOOLS_ACCESS_* */
} astools_fs_perm;

typedef struct {
  astools_fs_perm *fs;
  size_t fs_len;
  bool net, proc;
  char **env;
  size_t env_len;
} astools_perms;

typedef struct {
  char *os;   /* "linux" | "macos" | "windows" */
  char *arch; /* "x86_64" | "arm64" */
  char **argv;
  size_t argv_len;
} astools_entry;

typedef struct {
  char *code;        /* "<tool>/<slug>" */
  char *description; /* may be NULL */
} astools_errdecl;

typedef struct {
  char *title;         /* may be NULL */
  xcdn_node_t *call;   /* owned; literal args object */
  xcdn_node_t *result; /* owned; literal result value; may be NULL */
  char *note;          /* may be NULL */
} astools_example;

struct astools_cmd {
  char *name;
  char *summary;
  char *description; /* may be NULL */
  bool read_only, destructive, idempotent, long_running, deprecated;
  int64_t timeout_ms; /* 0 = unset */
  astools_param *params;
  size_t params_len;
  astools_type *returns_type; /* may be NULL */
  char *returns_desc;         /* may be NULL */
  astools_errdecl *errors;
  size_t errors_len;
  astools_example *examples;
  size_t examples_len;
};

typedef struct astools_manifest {
  int manifest_version;
  char *id;
  char *version;
  char *title;
  char *summary;
  char *description; /* may be NULL */
  int kind;          /* ASTOOLS_KIND_* */
  char **platforms;
  size_t platforms_len;
  int mode; /* ASTOOLS_MODE_* */
  astools_entry *entries;
  size_t entries_len;
  int64_t parallel;           /* persistent; default 1 */
  int64_t idle_timeout_ms;    /* persistent; default 120000 */
  int64_t startup_timeout_ms; /* persistent; default 10000 */
  astools_perms perms;
  astools_cmd *commands;
  size_t commands_len;
} astools_manifest;

/* Parse + schema-validate one manifest document. workspace expands
 * "${workspace}" in permission paths (may be NULL to leave verbatim).
 * On error returns NULL with a malloc'd message in *err_msg. */
astools_manifest *astools_manifest_parse(const char *text, size_t len,
                                         const char *workspace,
                                         char **err_msg);
void astools_manifest_free(astools_manifest *m);

/* Find a command by name; NULL if absent. */
const astools_cmd *astools_manifest_cmd(const astools_manifest *m,
                                        const char *name);

/* Lint (astools-check): appends one "warn: ..." / "error: ..." line per
 * finding; returns the number of error-level findings. */
int astools_manifest_lint(const astools_manifest *m, astools_buf *out);

/* Canonical re-serialization of a manifest (astools_tool_manifest API). */
astools_err astools_manifest_render(const astools_manifest *m, char **out);

/* ═══════════════════════ semver.c ═══════════════════════ */

typedef struct {
  int64_t major, minor, patch;
  char *prerelease; /* NULL = release; without leading '-' */
} astools_semver;

bool astools_semver_parse(const char *s, astools_semver *out);
/* Full SemVer 2.0.0 precedence; build metadata ignored. */
int  astools_semver_cmp(const astools_semver *a, const astools_semver *b);
void astools_semver_free(astools_semver *v);

/* ═══════════════════════ registry.c ═══════════════════════ */

#define ASTOOLS_TRUST_STANDARD 0
#define ASTOOLS_TRUST_FULL 1

/* Lockfile verdict for a tool under the current pinning mode. */
typedef enum {
  ASTOOLS_LOCK_UNLISTED = 0, /* no entry */
  ASTOOLS_LOCK_OK,           /* hashes match */
  ASTOOLS_LOCK_MISMATCH      /* entry present, hashes differ */
} astools_lock_state;

struct astools_tool {
  astools_manifest *m;
  char *pkg_dir; /* absolute package directory */
  int trust;     /* ASTOOLS_TRUST_* */
  bool available;
  bool enabled;        /* host toggle AND pinning gate */
  bool host_disabled;  /* astools_tool_enable(off) sticky across refresh */
  astools_lock_state lock_state;
  const astools_entry *entry; /* selected for current platform; borrowed */
  uint8_t content_sha256[32]; /* manifest + local runtime entry bytes */
  astools_semver ver;
  int refcount; /* 1 owned by table; invocations add refs */
};

/* Refcounting: table owns one ref. unref frees at zero. Thread-safe via
 * c->lock for ref (resolve takes it) and an internal atomic-ish mutex for
 * unref; implemented in registry.c. */
void astools_tool_ref(astools_tool *t);
void astools_tool_unref(astools_tool *t);

/* Full rescan of all roots; publishes the new table under c->lock,
 * unrefs replaced descriptors, updates stats + last_refresh. Returns
 * ASTOOLS_OK even when individual packages were rejected (logged). Sets
 * *out_changed = 1 when the published table differs (list_changed). */
astools_err astools_registry_scan(astools_ctx *c, int *out_changed);

/* Resolve "id" or "id@version" to a pinned descriptor (adds a ref; caller
 * unrefs). Applies config pins for bare refs. ASTOOLS_ERR_NOT_FOUND when
 * absent/unavailable on this platform; ASTOOLS_ERR_DENIED when disabled
 * or blocked by pinning "enforce". */
astools_err astools_registry_resolve(astools_ctx *c, const char *ref,
                                     astools_tool **out);

/* Re-check current on-disk bytes immediately before execution when pinning
 * is enforced.  This closes the interval between a file change and the next
 * registry poll. */
astools_err astools_registry_revalidate(astools_ctx *c,
                                        const astools_tool *t);

/* Current platform strings, compile-time detected. */
const char *astools_plat_os(void);   /* "linux" | "macos" | "windows" */
const char *astools_plat_arch(void); /* "x86_64" | "arm64" */

/* Poll support: 1 when registry.watch=poll and poll_interval elapsed.  The
 * scan itself hashes relevant contents, avoiding timestamp granularity bugs. */
int astools_registry_poll_due(astools_ctx *c);

/* ═══════════════════════ lockfile.c ═══════════════════════ */

typedef struct {
  char *path;
  uint8_t sha256[32];
} astools_lock_artifact;

typedef struct {
  char *id;
  char *version;
  uint8_t manifest_sha256[32];
  astools_lock_artifact *artifacts;
  size_t artifacts_len;
  astools_time approved_at;
} astools_lock_entry;

typedef struct {
  astools_lock_entry *entries;
  size_t len;
} astools_lockfile;

/* Load (missing file => empty lockfile, OK). path resolved against the
 * config dir when relative. */
astools_err astools_lockfile_load(const char *path, astools_lockfile *out);
void        astools_lockfile_free(astools_lockfile *lf);

/* Compare the package on disk with its lockfile entry. Hashes the
 * manifest text plus every existing entry argv[0] artifact. */
astools_lock_state astools_lockfile_check(const astools_lockfile *lf,
                                          const char *id, const char *version,
                                          const char *pkg_dir,
                                          const astools_manifest *m);

/* Recompute hashes for the package and rewrite the lockfile atomically
 * (add or replace the entry). now = approval timestamp. */
astools_err astools_lockfile_approve(const char *path, const char *id,
                                     const char *version, const char *pkg_dir,
                                     const astools_manifest *m,
                                     astools_time now);

/* ═══════════════════════ config.c ═══════════════════════ */

#define ASTOOLS_PIN_OFF 0
#define ASTOOLS_PIN_WARN 1
#define ASTOOLS_PIN_ENFORCE 2

#define ASTOOLS_SB_NONE 0
#define ASTOOLS_SB_BASIC 1
#define ASTOOLS_SB_STRICT 2

#define ASTOOLS_RESVAL_OFF 0
#define ASTOOLS_RESVAL_WARN 1
#define ASTOOLS_RESVAL_ENFORCE 2

typedef struct {
  char *path;
  int trust; /* ASTOOLS_TRUST_* */
} astools_root;

typedef struct {
  char *tool;
  char *version;
} astools_pin;

/* Per-tool host grant. */
typedef struct {
  char *tool;
  astools_fs_perm *fs;
  size_t fs_len;
  bool net, proc;
  char **env;
  size_t env_len;
} astools_grant;

typedef struct {
  /* registry */
  astools_root *reg_paths;
  size_t reg_paths_len;
  bool watch_poll;
  int64_t poll_interval_ms;
  int pinning; /* ASTOOLS_PIN_* */
  char *lock_path;
  astools_pin *pins;
  size_t pins_len;
  /* workspace */
  char *workspace_root;
  char *scratch; /* NULL = <root>/.astools/tmp */
  /* sandbox */
  int sandbox_level; /* ASTOOLS_SB_* */
  bool strict_fallback_reject;
  bool allow_library;
  /* grants */
  int workspace_access; /* 0 none | ASTOOLS_ACCESS_READ | _RW */
  astools_grant *tool_grants;
  size_t tool_grants_len;
  /* invocation */
  int64_t timeout_ms;
  int max_concurrent;
  int64_t max_output_bytes;
  int64_t stderr_max_bytes;
  int result_validation; /* ASTOOLS_RESVAL_* */
  /* catalog */
  int catalog_level; /* astools_catalog_level */
  size_t char_budget;
  char **priority;
  size_t priority_len;
  /* mcp */
  bool mcp_management;
  /* logging — field names shared with log.c */
  char *log_path;
  int log_level;
  int64_t log_max_size_kb;
  int log_max_files;
  bool log_sync;
  bool audit;
  /* derived */
  char *config_dir; /* dir of config file (or cwd); for relative paths */
} astools_config;

void        astools_config_defaults(astools_config *cfg);
/* Load and merge over defaults. Missing file with explicit_path=false is
 * OK (defaults). On parse/schema error returns ASTOOLS_ERR_CONFIG with a
 * malloc'd message. */
astools_err astools_config_load(const char *path, bool explicit_path,
                                astools_config *cfg, char **err_msg);
void        astools_config_free(astools_config *cfg);

/* ═══════════════════════ path.c ═══════════════════════ */

/* Canonicalize: join relative input to workspace, resolve
 * symlinks/dot segments. When missing_ok (write + !must_exist), the
 * deepest existing ancestor is canonicalized and the remainder
 * re-appended (the remainder must not contain ".."). Returns malloc'd
 * absolute path; ASTOOLS_ERR_NOT_FOUND when required to exist. */
astools_err astools_path_canonical(const char *workspace, const char *in,
                                   bool missing_ok, char **out);

/* 1 if path lies under prefix (component-boundary aware; equal counts). */
int astools_path_under(const char *path, const char *prefix);

/* ═══════════════════════ policy.c ═══════════════════════ */

typedef struct {
  astools_fs_perm *fs; /* canonical absolute prefixes */
  size_t fs_len;
  bool net, proc;
  char **env;
  size_t env_len;
} astools_effective;

/* manifest request ∩ host grants (workspace auto-grant + grants.tools). */
astools_err astools_policy_effective(astools_ctx *c, const astools_tool *t,
                                     astools_effective *out);
void astools_effective_free(astools_effective *eff);

/* Pre-flight (step 3 +): canonicalize every path-typed argument
 * (recursively, mutating args in place so tools receive canonical
 * absolute paths), check fs coverage, then net/proc/env requirements.
 * On denial returns ASTOOLS_ERR_DENIED with a malloc'd actionable message
 * naming the missing grant. */
astools_err astools_policy_preflight(astools_ctx *c, const astools_tool *t,
                                     const astools_cmd *cmd,
                                     xcdn_node_t *args,
                                     const astools_effective *eff,
                                     char **deny_msg);

/* ═══════════════════════ sandbox_posix.c ═══════════════════════ */

typedef struct {
  int level;         /* effective level after fallback */
  char **argv;       /* NULL-terminated exec vector (jail-wrapped in strict);
                        owned */
  char **envp;       /* NULL-terminated scrubbed environment; owned */
  char *scratch_dir; /* created private cwd; owned */
  int64_t limit_cpu_seconds, limit_mem_bytes, limit_nproc;
} astools_sandbox_setup;

/* Build the sandbox for one invocation: effective level (tool root may
 * not raise above cfg), scrubbed env (granted vars + PATH/HOME/TMPDIR +
 * ASTOOLS_* control vars), scratch dir creation, jail wrapping.
 * entry_argv is the resolved absolute argv of the tool. */
astools_err astools_sandbox_prepare(astools_ctx *c, const astools_tool *t,
                                    const astools_effective *eff,
                                    const char *invocation_id,
                                    char *const *entry_argv,
                                    astools_sandbox_setup *out);
/* Delete scratch (unless keep) and free members. */
void astools_sandbox_cleanup(astools_ctx *c, astools_sandbox_setup *s,
                             bool keep_scratch);

/* Honest per-feature enforcement report for this platform. */
astools_err astools_sandbox_caps_impl(int strict, astools_sandbox_caps *out);

/* RLIMIT_NPROC value for a spawned tool: the account's current task count
 * plus a fixed headroom, because the kernel charges the limit per real UID
 * across the whole system rather than per process tree. A fixed cap would
 * deny fork() outright to every tool on a host that already runs more tasks
 * than the cap. ASTOOLS_ERR_UNSUPPORTED where usage cannot be read, meaning
 * no cap should be applied. */
astools_err astools_sandbox_nproc_cap(int64_t *out);

/* Tasks a tool may create beyond what the account already runs. */
#define ASTOOLS_NPROC_HEADROOM 256

/* Resolve entry argv[0] against the package dir (or PATH/absolute for
 * full-trust roots) into a full exec vector; malloc'd NULL-terminated. */
astools_err astools_entry_resolve_argv(const astools_tool *t, char ***out);
void        astools_argv_free(char **argv);

/* ═══════════════════════ proto.c ═══════════════════════ */

/* Serialize one xCDN node/value standalone (compact single line unless
 * pretty). malloc'd. */
char *astools_xcdn_write_node(const xcdn_node_t *n, bool pretty);
char *astools_xcdn_write_value(const xcdn_value_t *v, bool pretty);

/* Parse text expecting at least one top-level value; *out_doc owns the
 * tree, *out_node borrows the first value. */
astools_err astools_xcdn_parse_first(const char *text, size_t len,
                                     xcdn_document_t **out_doc,
                                     xcdn_node_t **out_node, char **err_msg);

/* Build one #tool_request. args is the validated+canonical args
 * object. deadline is an absolute unix time. */
astools_err astools_proto_request(const astools_tool *t,
                                  const astools_cmd *cmd,
                                  const xcdn_node_t *args,
                                  const char *invocation_id,
                                  astools_time deadline,
                                  int64_t max_output_bytes,
                                  const astools_effective *eff,
                                  const char *workspace, const char *scratch,
                                  char **out_text);

/* Parse one #tool_response; fills ok/result/error into r (malloc'd
 * strings). Wrong invocation_id or malformed shape =>
 * ASTOOLS_ERR_PROTOCOL. expect_id may be NULL (persistent reader matches
 * ids itself via *out_id). */
astools_err astools_proto_parse_response(const char *text, size_t len,
                                         const char *expect_id,
                                         char **out_id, astools_result *r,
                                         char **err_msg);

/* Persistent-mode messages. */
astools_err astools_proto_parse_hello(const char *text, size_t len,
                                      const astools_tool *t, char **err_msg);
char       *astools_proto_cancel(const char *invocation_id);

/* ═══════════════════════ invoke.c ═══════════════════════ */

/* cancel_task (may be NULL): its cancelled field is read under the task
 * mutex, so cancellation is race-free in strict C99. */
astools_err astools_invoke_impl(astools_ctx *c, const char *ref,
                                const char *command, const char *args_xcdn,
                                uint32_t deadline_ms,
                                astools_task *cancel_task,
                                astools_result *out);

/* Validation-only path (public astools_validate_args). */
astools_err astools_validate_impl(astools_ctx *c, const char *ref,
                                  const char *command,
                                  const char *args_xcdn);

/* ═══════════════════════ worker.c ═══════════════════════ */

/* One oneshot execution: spawn under setup, write request, pump stdio
 * with caps (max_output_bytes / stderr cap), enforce deadline_mono (an
 * os_monotonic_ms instant), kill on overrun, reap. Fills r (parsing the
 * response), *exit_code, and returns the engine verdict. stderr excerpt
 * is captured into *stderr_cap (malloc'd, may be NULL). */
astools_err astools_exec_oneshot(astools_ctx *c,
                                 const astools_sandbox_setup *setup,
                                 const char *request_text,
                                 const char *invocation_id,
                                 int64_t deadline_mono,
                                 int64_t max_output_bytes,
                                 int64_t stderr_max_bytes,
                                 astools_task *cancel_task,
                                 astools_result *r, int *exit_code,
                                 char **stderr_cap);

/* Persistent dispatch: acquire/start the tool's process (handshake with
 * startup_timeout), send the request, wait for the matching response or
 * deadline (cancel + grace + kill/restart on overrun). */
astools_err astools_exec_persistent(astools_ctx *c, astools_tool *t,
                                    const astools_sandbox_setup *setup,
                                    const char *request_text,
                                    const char *invocation_id,
                                    int64_t deadline_mono,
                                    astools_task *cancel_task,
                                    astools_result *r);

/* Library dispatch (kind "library"): dlopen once per tool, call
 * vtable->invoke on the calling thread. */
astools_err astools_exec_library(astools_ctx *c, astools_tool *t,
                                 const char *request_text,
                                 astools_result *r);

/* Supervisor lifecycle: registry poll + persistent idle reaping + async
 * task queue. */
astools_err astools_worker_start(astools_ctx *c);
void        astools_worker_stop(astools_ctx *c);
/* Run due background work now (astools_tick / ASTOOLS_NO_THREADS). */
astools_err astools_worker_tick(astools_ctx *c);

/* Slot admission (invocation.max_concurrent). Blocks until a slot or
 * deadline_mono (<=0: forever). ASTOOLS_ERR_TIMEOUT on overrun. */
astools_err astools_slot_acquire(astools_ctx *c, int64_t deadline_mono);
void        astools_slot_release(astools_ctx *c);

/* Async task object (astools_invoke_async). */
struct astools_task {
  astools_ctx *c;
  char *ref, *command, *args;
  uint32_t deadline_ms;
  os_mutex mu;
  os_cond cv;
  bool done;
  bool cancelled;
  astools_err verdict;
  astools_result result;
  os_thread thread;
  bool thread_valid;
};

/* ═══════════════════════ catalog.c / gbnf.c / callline.c ═══════════ */

astools_err astools_catalog_render(astools_ctx *c, astools_catalog_level lvl,
                                   size_t char_budget, char **out);
astools_err astools_gbnf_render(astools_ctx *c, char **out);

astools_err astools_callline_parse(const char *model_output, char **out_ref,
                                   char **out_command, char **out_args);
astools_err astools_callline_format(const char *ref, const char *command,
                                    const astools_result *r, char **out_line);

/* ═══════════════════════ jscm.c ═══════════════════════ */

/* JSON Schema object (compact JSON text) for a command's parameters. */
astools_err astools_jschema_input(const astools_cmd *cmd, char **out_json);
/* Composed MCP description: tool summary + command description +
 * rendered examples + permission note. */
astools_err astools_mcp_description(const astools_tool *t,
                                    const astools_cmd *cmd, char **out_text);

/* ═══════════════════════ log.c ═══════════════════════ */

astools_err astools_log_open(astools_ctx *c);
void        astools_log_close(astools_ctx *c);
/* subsys: "registry","manifest","invoke","proc","sandbox","catalog",
 * "policy","config","mcp","worker". Never fails the caller. */
void astools_log(astools_ctx *c, int level, const char *subsys,
                 const char *fmt, ...);

/* Audit trail: appends one #invocation record to audit.xcdn next to
 * the config (config_dir). args_sha256 of the canonical args text. */
void astools_audit_append(astools_ctx *c, const char *tool,
                          const char *version, const char *command, bool ok,
                          const char *error_code, uint64_t duration_ms,
                          const uint8_t args_sha256[32]);

/* ═══════════════════════ the context ═══════════════════════ */

typedef struct astools_pproc astools_pproc; /* worker.c-private */

struct astools_ctx {
  astools_config cfg;
  char *workspace;    /* canonical absolute */
  char *scratch_base; /* canonical absolute (may not exist yet) */

  astools_clock clock; /* wall; injectable */
  int64_t (*mono_ms)(void *ud); /* monotonic; injectable */
  void *mono_ud;

  /* registry (guarded by lock) */
  astools_tool **tools;
  size_t tools_n, tools_cap;
  astools_lockfile lockfile;
  os_rwlock lock;
  int64_t last_scan_mono;
  uint64_t scan_fingerprint; /* compact digest of published tool contents */
  astools_time last_refresh_unix;
  bool tools_changed_flag; /* set by scan for MCP list_changed; consumer
                              clears */

  /* invocation slots */
  os_mutex slot_mu;
  os_cond slot_cv;
  int slots_used;

  /* persistent processes (guarded by pp_mu; worker reaps idle) */
  astools_pproc *pprocs;
  os_mutex pp_mu;

  /* supervisor */
  os_thread worker;
  bool worker_running;
  bool stop_worker;
  os_mutex ev_mu;
  os_cond ev_cv;

  /* error reporting */
  os_mutex err_mu;
  char err_buf[512];

  /* stats (guarded by lock) */
  astools_stats stats;

  /* logging */
  FILE *log_fp;
  uint64_t log_size;
  os_mutex log_mu;
  astools_log_fn log_cb;
  void *log_ud;
  os_mutex audit_mu;

  bool no_threads;
};

/* Store a formatted message into c->err_buf (thread-safe) and return e —
 * `return astools_seterr(c, ASTOOLS_ERR_X, "...")` idiom. NULL c is
 * tolerated. */
astools_err astools_seterr(astools_ctx *c, astools_err e, const char *fmt,
                           ...);

/* Monotonic now via the injectable hook. */
int64_t astools_mono(astools_ctx *c);

#ifdef __cplusplus
}
#endif

#endif /* ASTOOLS_INTERNAL_H */
