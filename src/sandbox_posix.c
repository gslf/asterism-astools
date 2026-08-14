/*
 * sandbox_posix.c — per-invocation sandbox assembly (§6, §14): entry argv
 * resolution under root trust, scrubbed environment, scratch directory,
 * resource-limit selection, Seatbelt jail wrapping (macOS strict), and the
 * honest capability report.
 *
 * Everything here fronts hostile input: manifests choose entry argv and env
 * grant names, so trust checks fail closed and reserved environment names
 * can never be overridden by a grant.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- growable NULL-terminated string vector ------------------------------ */

/*
 * Invariant: after every successful push the array is NULL-terminated, so a
 * partially built vector can always be released with astools_argv_free.
 * Takes ownership of item; item == NULL reports the allocation failure of
 * the caller's producer expression.
 */
static astools_err strv_push(char ***arr, size_t *len, size_t *cap,
                             char *item) {
  char **p;
  size_t ncap;
  if (!item) return ASTOOLS_ERR_NOMEM;
  if (*len > SIZE_MAX / sizeof *p - 2) {
    free(item);
    return ASTOOLS_ERR_NOMEM;
  }
  if (*len + 2 > *cap) {
    ncap = *cap ? *cap * 2 : 16;
    while (ncap < *len + 2) ncap *= 2;
    p = realloc(*arr, ncap * sizeof *p);
    if (!p) {
      free(item);
      return ASTOOLS_ERR_NOMEM;
    }
    *arr = p;
    *cap = ncap;
  }
  (*arr)[(*len)++] = item;
  (*arr)[*len] = NULL;
  return ASTOOLS_OK;
}

void astools_argv_free(char **argv) {
  size_t i;
  if (!argv) return;
  for (i = 0; argv[i] != NULL; i++) free(argv[i]);
  free(argv);
}

/* ---- entry argv resolution (§3.2, §4.1) ---------------------------------- */

static int path_has_sep(const char *p) { return strchr(p, '/') != NULL; }

/* 1 when the path contains a ".." component (would escape pkg_dir). */
static int path_has_dotdot(const char *p) {
  size_t i = 0;
  while (p[i] != '\0') {
    size_t start = i;
    while (p[i] != '\0' && p[i] != '/') i++;
    if (i - start == 2 && p[start] == '.' && p[start + 1] == '.') return 1;
    if (p[i] == '/') i++;
  }
  return 0;
}

astools_err astools_entry_resolve_argv(const astools_tool *t, char ***out) {
  const astools_entry *en;
  const char *a0;
  char **argv;
  size_t i, n;

  if (!t || !out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  en = t->entry;
  if (!en || !en->argv || en->argv_len == 0 || !en->argv[0] ||
      en->argv[0][0] == '\0')
    return ASTOOLS_ERR_INVALID;
  a0 = en->argv[0];

  n = en->argv_len;
  argv = calloc(n + 1, sizeof *argv);
  if (!argv) return ASTOOLS_ERR_NOMEM;

  if (t->trust == ASTOOLS_TRUST_STANDARD) {
    /* Standard trust: argv[0] must stay inside the package (§4.1). Bare
     * names (PATH lookup), absolute paths and ".." escapes are refused. */
    if (os_path_is_abs(a0) || !path_has_sep(a0) || path_has_dotdot(a0)) {
      free(argv);
      return ASTOOLS_ERR_DENIED;
    }
    argv[0] = os_path_join(t->pkg_dir, a0);
  } else {
    /* Full trust: absolute kept verbatim; bare names left for the PATH
     * search in os_proc_spawn; relative-with-separator is still resolved
     * against the package dir. */
    if (os_path_is_abs(a0) || !path_has_sep(a0))
      argv[0] = astools_strdup(a0);
    else
      argv[0] = os_path_join(t->pkg_dir, a0);
  }
  if (!argv[0]) {
    free(argv);
    return ASTOOLS_ERR_NOMEM;
  }
  for (i = 1; i < n; i++) {
    argv[i] = astools_strdup(en->argv[i] ? en->argv[i] : "");
    if (!argv[i]) {
      astools_argv_free(argv);
      return ASTOOLS_ERR_NOMEM;
    }
  }
  *out = argv;
  return ASTOOLS_OK;
}

/* ---- environment scrub (§5.2) -------------------------------------------- */

/* malloc'd "name=value". */
static char *env_kv(const char *name, const char *value) {
  size_t n = strlen(name), v = strlen(value);
  char *s;
  if (n > SIZE_MAX - v - 2) return NULL;
  s = malloc(n + v + 2);
  if (!s) return NULL;
  memcpy(s, name, n);
  s[n] = '=';
  memcpy(s + n + 1, value, v + 1);
  return s;
}

/*
 * Names the runtime itself sets (§5.2). An env grant must never override
 * them: duplicate envp entries have libc-defined precedence, so a hostile
 * manifest granting "PATH" or "ASTOOLS_SCRATCH" could otherwise undo the
 * scrub. Names containing '=' would smuggle an extra variable.
 */
static bool env_name_reserved(const char *name) {
  if (!name || name[0] == '\0') return true;
  if (strchr(name, '=') != NULL) return true;
  if (strcmp(name, "PATH") == 0 || strcmp(name, "HOME") == 0 ||
      strcmp(name, "TMPDIR") == 0)
    return true;
  if (strncmp(name, "ASTOOLS_", 8) == 0) return true;
  return false;
}

/* ---- jail discovery + Seatbelt profile (macOS strict, §6, §14) ----------- */

#if defined(__APPLE__)

/*
 * Jail helper lookup: $ASTOOLS_JAIL when it names an existing file, else
 * <exedir>/astools-jail when present. NULL = strict not enforceable here.
 * (An ASTOOLS_JAIL pointing at a missing file falls through to the exedir
 * default rather than silently selecting a binary that cannot exec.)
 */
static char *sandbox_jail_path(void) {
  char *p, *exe, *dir, *joined;
  const char *slash;
  p = os_env_dup("ASTOOLS_JAIL");
  if (p) {
    if (os_file_exists(p)) return p;
    free(p);
  }
  if (os_exe_path(&exe) != ASTOOLS_OK) return NULL;
  slash = strrchr(exe, '/');
  if (!slash) {
    free(exe);
    return NULL;
  }
  dir = (slash == exe) ? astools_strdup("/")
                       : astools_strndup(exe, (size_t)(slash - exe));
  free(exe);
  if (!dir) return NULL;
  joined = os_path_join(dir, "astools-jail");
  free(dir);
  if (!joined) return NULL;
  if (os_file_exists(joined)) return joined;
  free(joined);
  return NULL;
}

/* Error-latching append: no-op once *e != ASTOOLS_OK. */
static void sb_add(astools_buf *b, astools_err *e, const char *s) {
  if (*e != ASTOOLS_OK) return;
  *e = astools_buf_appends(b, s);
}

/*
 * Append one SBPL string literal, escaping '\' and '"'. Control bytes in a
 * path cannot be represented safely in SBPL, so they fail the profile
 * (fail closed — never emit a profile that might be misparsed).
 */
static void sb_quote(astools_buf *b, astools_err *e, const char *s) {
  size_t i;
  if (*e != ASTOOLS_OK) return;
  *e = astools_buf_appendc(b, '"');
  for (i = 0; *e == ASTOOLS_OK && s[i] != '\0'; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch < 0x20 || ch == 0x7f) {
      *e = ASTOOLS_ERR_INVALID;
      return;
    }
    if (ch == '"' || ch == '\\') *e = astools_buf_appendc(b, '\\');
    if (*e == ASTOOLS_OK) *e = astools_buf_appendc(b, (char)ch);
  }
  if (*e == ASTOOLS_OK) *e = astools_buf_appendc(b, '"');
}

/*
 * Seatbelt profile generator. Shape (deny-by-default, §6):
 *
 *   (version 1)
 *   (deny default)
 *   (allow process-exec*) (allow process-fork)   ; run the tool binary
 *   (allow sysctl-read) (allow mach-lookup)      ; libSystem startup
 *   (allow signal (target same-sandbox))
 *   (allow file-read* <runtime prefixes: /usr/lib /System /usr/share
 *     /private/var/db/dyld /Library/Preferences/Logging /dev/null
 *     /dev/urandom /dev/random /dev/fd> (subpath <pkg_dir>))
 *   (allow file-write-data (literal "/dev/null"))
 *   (allow file-read* (subpath <scratch>)) (allow file-write* (subpath ..))
 *   per effective fs grant: READ  => (allow file-read*  (subpath P))
 *                           WRITE => (allow file-write* (subpath P))
 *   net grant => (allow network*); otherwise deny-default blocks it.
 */
static astools_err sandbox_sbpl_profile(const char *pkg_dir,
                                        const char *scratch,
                                        const astools_effective *eff,
                                        char **out) {
  astools_buf b;
  astools_err e = ASTOOLS_OK;
  size_t i;

  *out = NULL;
  if (!pkg_dir || pkg_dir[0] == '\0' || !scratch || scratch[0] == '\0')
    return ASTOOLS_ERR_INVALID;

  astools_buf_init(&b);
  sb_add(&b, &e,
         "(version 1)\n"
         "(deny default)\n"
         "(allow process-exec*)\n"
         "(allow process-fork)\n"
         "(allow sysctl-read)\n"
         "(allow mach-lookup)\n"
         "(allow signal (target same-sandbox))\n"
         "(allow file-read*\n"
         /* dyld's CacheFinder stats the volume root itself; without the
          * "/" literal every process aborts before main (verified on
          * macOS, dyld4 ignition_halt). preboot/var-db cover the shared
          * cache and system databases across macOS versions. */
         "  (literal \"/\")\n"
         "  (subpath \"/usr/lib\")\n"
         "  (subpath \"/System\")\n"
         "  (subpath \"/usr/share\")\n"
         "  (subpath \"/private/preboot\")\n"
         "  (subpath \"/private/var/db/dyld\")\n"
         "  (subpath \"/Library/Preferences/Logging\")\n"
         "  (literal \"/dev/null\")\n"
         "  (literal \"/dev/urandom\")\n"
         "  (literal \"/dev/random\")\n"
         "  (subpath \"/dev/fd\")\n"
         "  (subpath ");
  sb_quote(&b, &e, pkg_dir);
  sb_add(&b, &e,
         "))\n"
         "(allow file-write-data (literal \"/dev/null\"))\n"
         "(allow file-read* (subpath ");
  sb_quote(&b, &e, scratch);
  sb_add(&b, &e, "))\n(allow file-write* (subpath ");
  sb_quote(&b, &e, scratch);
  sb_add(&b, &e, "))\n");
  for (i = 0; eff && i < eff->fs_len; i++) {
    const astools_fs_perm *p = &eff->fs[i];
    if (!p->path || p->path[0] == '\0') continue;
    if (p->access & ASTOOLS_ACCESS_READ) {
      sb_add(&b, &e, "(allow file-read* (subpath ");
      sb_quote(&b, &e, p->path);
      sb_add(&b, &e, "))\n");
    }
    if (p->access & ASTOOLS_ACCESS_WRITE) {
      sb_add(&b, &e, "(allow file-write* (subpath ");
      sb_quote(&b, &e, p->path);
      sb_add(&b, &e, "))\n");
    }
  }
  if (eff && eff->net) sb_add(&b, &e, "(allow network*)\n");

  if (e != ASTOOLS_OK) {
    astools_buf_free(&b);
    return e;
  }
  *out = astools_buf_detach(&b);
  return *out ? ASTOOLS_OK : ASTOOLS_ERR_NOMEM;
}

#endif /* __APPLE__ */

/* ---- sandbox preparation (§6) -------------------------------------------- */

astools_err astools_sandbox_prepare(astools_ctx *c, const astools_tool *t,
                                    const astools_effective *eff,
                                    const char *invocation_id,
                                    char *const *entry_argv,
                                    astools_sandbox_setup *out) {
  int level;
  char *jail = NULL;
  char *scratch = NULL;
  char **envp = NULL, **argv = NULL;
  size_t envn = 0, envcap = 0, argn = 0, argcap = 0;
  size_t i, n;
  bool scratch_made = false;
  astools_err e;

  if (!c || !t || !eff || !invocation_id || invocation_id[0] == '\0' ||
      !entry_argv || !entry_argv[0] || !out)
    return astools_seterr(c, ASTOOLS_ERR_INVALID,
                          "sandbox: invalid prepare arguments");
  memset(out, 0, sizeof *out);
#if !defined(__APPLE__)
  (void)t; /* pkg_dir only feeds the Seatbelt profile */
#endif

  /* 1. Effective level. Strict needs kernel enforcement this build can
   * actually deliver; otherwise reject or degrade per config (§6). */
  level = c->cfg.sandbox_level;
  if (level == ASTOOLS_SB_STRICT) {
#if defined(__APPLE__)
    jail = sandbox_jail_path();
    if (!jail) {
      if (c->cfg.strict_fallback_reject)
        return astools_seterr(
            c, ASTOOLS_ERR_UNSUPPORTED,
            "sandbox strict unavailable: astools-jail helper not found "
            "(missing: fs confinement, net deny, privilege drop)");
      astools_log(c, ASTOOLS_LOG_WARN, "sandbox",
                  "strict requested but astools-jail helper not found; "
                  "degrading to basic (missing: fs confinement, net deny, "
                  "privilege drop)");
      level = ASTOOLS_SB_BASIC;
    }
#else
    if (c->cfg.strict_fallback_reject)
      return astools_seterr(
          c, ASTOOLS_ERR_UNSUPPORTED,
          "sandbox strict unavailable in this build (missing: Landlock fs "
          "confinement, seccomp syscall filter, net deny)");
    astools_log(c, ASTOOLS_LOG_WARN, "sandbox",
                "strict requested but kernel enforcement is not wired in "
                "this build; degrading to basic (missing: Landlock fs "
                "confinement, seccomp syscall filter, net deny)");
    level = ASTOOLS_SB_BASIC;
#endif
  }

  /* 2. Private scratch dir = cwd, HOME and TMPDIR of the child (§5.2). */
  scratch = os_path_join(c->scratch_base, invocation_id);
  if (!scratch) {
    e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "sandbox: out of memory");
    goto fail;
  }
  e = os_mkdir_p(scratch);
  if (e != ASTOOLS_OK) {
    e = astools_seterr(c, e, "sandbox: cannot create scratch dir %s",
                       scratch);
    goto fail;
  }
  scratch_made = true;

  /* 3. Environment, rebuilt from scratch — the host environment never
   * leaks except through explicit env grants (§5.2). */
  e = strv_push(&envp, &envn, &envcap,
                astools_strdup("PATH=/usr/bin:/bin:/usr/sbin:/sbin"));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("HOME", scratch));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("TMPDIR", scratch));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, astools_strdup("ASTOOLS_PROTOCOL=1"));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap,
                  env_kv("ASTOOLS_INVOCATION_ID", invocation_id));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap,
                  env_kv("ASTOOLS_WORKSPACE",
                         c->workspace ? c->workspace : ""));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("ASTOOLS_SCRATCH", scratch));
  if (e != ASTOOLS_OK) {
    e = astools_seterr(c, e, "sandbox: cannot build environment");
    goto fail;
  }
  for (i = 0; i < eff->env_len; i++) {
    const char *name = eff->env[i];
    char *v, *kv;
    if (env_name_reserved(name)) {
      astools_log(c, ASTOOLS_LOG_DEBUG, "sandbox",
                  "env grant '%s' ignored (reserved name)",
                  name ? name : "");
      continue;
    }
    v = os_env_dup(name);
    if (!v) continue; /* absent in host env => absent for the tool */
    kv = env_kv(name, v);
    free(v);
    e = strv_push(&envp, &envn, &envcap, kv);
    if (e != ASTOOLS_OK) {
      e = astools_seterr(c, e, "sandbox: cannot build environment");
      goto fail;
    }
  }

  /* 5. Exec vector; strict on macOS wraps it in the Seatbelt jail and
   * hands the profile over via ASTOOLS_JAIL_PROFILE. */
#if defined(__APPLE__)
  if (level == ASTOOLS_SB_STRICT) {
    char *profile = NULL, *kv, *j;
    e = sandbox_sbpl_profile(t->pkg_dir, scratch, eff, &profile);
    if (e != ASTOOLS_OK) {
      e = astools_seterr(c, e, "sandbox: cannot generate Seatbelt profile");
      goto fail;
    }
    kv = env_kv("ASTOOLS_JAIL_PROFILE", profile);
    free(profile);
    e = strv_push(&envp, &envn, &envcap, kv);
    if (e != ASTOOLS_OK) {
      e = astools_seterr(c, e, "sandbox: cannot build environment");
      goto fail;
    }
    j = jail;
    jail = NULL; /* ownership moves into argv (or is freed by push) */
    e = strv_push(&argv, &argn, &argcap, j);
    if (e == ASTOOLS_OK)
      e = strv_push(&argv, &argn, &argcap, astools_strdup("--"));
    if (e != ASTOOLS_OK) {
      e = astools_seterr(c, e, "sandbox: cannot build exec vector");
      goto fail;
    }
  }
#endif
  for (n = 0; entry_argv[n] != NULL; n++) {}
  for (i = 0; i < n; i++) {
    e = strv_push(&argv, &argn, &argcap, astools_strdup(entry_argv[i]));
    if (e != ASTOOLS_OK) {
      e = astools_seterr(c, e, "sandbox: cannot build exec vector");
      goto fail;
    }
  }

  /* 4. Limits. CPU stays 0: invoke derives RLIMIT_CPU from the deadline
   * and the worker applies it at spawn. RLIMIT_AS is unreliable on macOS
   * (§14: system frameworks mmap far past any sane cap) and RLIMIT_NPROC
   * is per-uid there (a cap would throttle the whole login session), so
   * both are Linux-only; level none is debug mode and stays unlimited. */
  out->limit_cpu_seconds = 0;
#if defined(__linux__)
  if (level != ASTOOLS_SB_NONE) {
    out->limit_mem_bytes = (int64_t)1 << 30;
    out->limit_nproc = 256;
  }
#endif

  out->level = level;
  out->argv = argv;
  out->envp = envp;
  out->scratch_dir = scratch;
  return ASTOOLS_OK;

fail:
  free(jail);
  astools_argv_free(argv);
  astools_argv_free(envp);
  if (scratch) {
    if (scratch_made) os_rmtree(scratch);
    free(scratch);
  }
  return e;
}

void astools_sandbox_cleanup(astools_ctx *c, astools_sandbox_setup *s,
                             bool keep_scratch) {
  if (!s) return;
  if (s->scratch_dir && !keep_scratch) {
    if (os_rmtree(s->scratch_dir) != ASTOOLS_OK)
      astools_log(c, ASTOOLS_LOG_DEBUG, "sandbox",
                  "scratch cleanup failed for %s", s->scratch_dir);
  }
  astools_argv_free(s->argv);
  astools_argv_free(s->envp);
  free(s->scratch_dir);
  memset(s, 0, sizeof *s);
}

/* ---- honest capability report (§6.5, §14) -------------------------------- */

astools_err astools_sandbox_caps_impl(int strict, astools_sandbox_caps *out) {
  if (!out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof *out);
  /* basic (POSIX): RLIMIT_CPU everywhere; RLIMIT_AS / RLIMIT_NPROC only
   * trustworthy on Linux (§14). fs/net policy is pre-flight only. */
  out->cpu_cap = 1;
#if defined(__linux__)
  out->memory_cap = 1;
  out->process_cap = 1;
#endif
#if defined(__APPLE__)
  /* strict on macOS: Seatbelt via astools-jail when discoverable — kernel
   * fs confinement, network deny and privilege drop; no syscall filter.
   * Without the helper, strict cannot add anything (honest zeros). */
  if (strict) {
    char *jail = sandbox_jail_path();
    if (jail) {
      out->fs_confinement = 1;
      out->net_deny = 1;
      out->privilege_drop = 1;
      free(jail);
    }
  }
#else
  /* strict on Linux in this build: Landlock/seccomp not wired yet, so the
   * report stays identical to basic (honest zeros). */
  (void)strict;
#endif
  return ASTOOLS_OK;
}
