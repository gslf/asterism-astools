/*
 * sandbox_win32.c — per-invocation sandbox assembly for Windows: entry
 * argv resolution under root trust, scrubbed environment, scratch
 * directory, Job-Object-backed resource limits, and the honest
 * capability report.
 *
 * Enforcement level: "basic" is real here — os_proc_spawn puts every tool
 * in its own Job Object, so the cpu / memory / process caps chosen below
 * are kernel-enforced. "strict" (fs confinement, network deny, syscall
 * filter, privilege drop) has no Windows jail yet and degrades to basic
 * (or is rejected, per strict_fallback_reject), exactly like the
 * platforms without a jail helper in sandbox_posix.c.
 *
 * The environment is rebuilt from scratch. Windows children additionally
 * need SystemRoot and friends for even libc-level startup, so those are
 * pinned from the host; scratch doubles as HOME/USERPROFILE and all three
 * temp spellings. Windows environment names are case-insensitive, so the
 * reserved-name check is too — a grant for "temp" or "systemroot" must
 * not undo the scrub.
 */

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "astools_internal.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

/* ---- growable NULL-terminated string vector ------------------------------ */

/* Same contract as sandbox_posix.c: always NULL-terminated after a
 * successful push; takes ownership of item; item == NULL reports the
 * caller's producer allocation failure. */
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

/* ---- entry argv resolution ---------------------------------- */

static int path_has_sep(const char *p) {
  return strchr(p, '/') != NULL || strchr(p, '\\') != NULL;
}

/* 1 when the path contains a ".." component (would escape pkg_dir). */
static int path_has_dotdot(const char *p) {
  size_t i = 0;
  while (p[i] != '\0') {
    size_t start = i;
    while (p[i] != '\0' && p[i] != '/' && p[i] != '\\') i++;
    if (i - start == 2 && p[start] == '.' && p[start + 1] == '.') return 1;
    if (p[i] != '\0') i++;
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
    /* Standard trust: argv[0] must stay inside the package. Bare
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

/* ---- environment scrub -------------------------------------------- */

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
 * Names the runtime itself sets (or that Windows program startup depends
 * on). Environment names are case-insensitive on Windows, so the check is
 * too — a hostile manifest granting "path" or "Temp" could otherwise undo
 * the scrub. Names containing '=' would smuggle an extra variable.
 */
static bool env_name_reserved(const char *name) {
  static const char *const reserved[] = {
      "PATH",       "HOME",        "TMPDIR",  "TEMP",    "TMP",
      "USERPROFILE", "SYSTEMROOT", "SYSTEMDRIVE", "WINDIR", "COMSPEC",
      "PATHEXT",    NULL};
  size_t i;
  if (!name || name[0] == '\0') return true;
  if (strchr(name, '=') != NULL) return true;
  for (i = 0; reserved[i] != NULL; i++)
    if (_stricmp(name, reserved[i]) == 0) return true;
  if (_strnicmp(name, "ASTOOLS_", 8) == 0) return true;
  return false;
}

/* Host system directories as malloc'd UTF-8 ("" on failure). */
static char *sysdir_dup(UINT (WINAPI *get)(LPWSTR, UINT)) {
  wchar_t w[MAX_PATH + 1];
  UINT n = get(w, MAX_PATH + 1);
  int u8n;
  char *s;
  if (n == 0 || n > MAX_PATH) return astools_strdup("");
  u8n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  if (u8n <= 0) return astools_strdup("");
  s = malloc((size_t)u8n);
  if (!s) return NULL;
  if (WideCharToMultiByte(CP_UTF8, 0, w, -1, s, u8n, NULL, NULL) <= 0) {
    free(s);
    return astools_strdup("");
  }
  return s;
}

/* ---- sandbox preparation -------------------------------------------- */

astools_err astools_sandbox_prepare(astools_ctx *c, const astools_tool *t,
                                    const astools_effective *eff,
                                    const char *invocation_id,
                                    char *const *entry_argv,
                                    astools_sandbox_setup *out) {
  int level;
  char *scratch = NULL;
  char *sys32 = NULL, *windir = NULL;
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

  /* 1. Effective level. Strict needs kernel fs/net enforcement that this
   * platform does not have wired in yet; reject or degrade per config. */
  level = c->cfg.sandbox_level;
  if (level == ASTOOLS_SB_STRICT) {
    if (c->cfg.strict_fallback_reject)
      return astools_seterr(
          c, ASTOOLS_ERR_UNSUPPORTED,
          "sandbox strict unavailable on Windows (missing: fs confinement, "
          "net deny, syscall filter, privilege drop)");
    astools_log(c, ASTOOLS_LOG_WARN, "sandbox",
                "strict requested but Windows has no jail wired in; "
                "degrading to basic (missing: fs confinement, net deny, "
                "syscall filter, privilege drop)");
    level = ASTOOLS_SB_BASIC;
  }

  /* 2. Private scratch dir = cwd, HOME and TEMP of the child. */
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
   * leaks except through explicit env grants and the system paths a
   * Windows process cannot start without. */
  sys32 = sysdir_dup(GetSystemDirectoryW);
  windir = sysdir_dup(GetSystemWindowsDirectoryW);
  if (!sys32 || !windir) {
    e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "sandbox: out of memory");
    goto fail;
  }
  {
    /* The POSIX scrub keeps /usr/bin:/bin, where the system git lives;
     * the Windows counterpart of that well-known location is
     * %ProgramFiles%\Git\cmd — appended only when it actually exists,
     * never the caller's PATH. */
    char gitdir[MAX_PATH + 16];
    size_t pn;
    char *pathv;
    char *comspec;
    gitdir[0] = '\0';
    {
      wchar_t wpf[MAX_PATH + 1];
      DWORD gn = GetEnvironmentVariableW(L"ProgramFiles", wpf, MAX_PATH + 1);
      if (gn > 0 && gn <= MAX_PATH) {
        wchar_t wgit[MAX_PATH + 16];
        (void)_snwprintf_s(wgit, MAX_PATH + 16, _TRUNCATE, L"%s\\Git\\cmd",
                           wpf);
        if (GetFileAttributesW(wgit) != INVALID_FILE_ATTRIBUTES)
          (void)WideCharToMultiByte(CP_UTF8, 0, wgit, -1, gitdir,
                                    sizeof gitdir, NULL, NULL);
      }
    }
    pn = strlen(sys32) + strlen(windir) + strlen(gitdir) + 3;
    pathv = malloc(pn);
    if (!pathv) {
      e = astools_seterr(c, ASTOOLS_ERR_NOMEM, "sandbox: out of memory");
      goto fail;
    }
    if (gitdir[0] != '\0')
      (void)snprintf(pathv, pn, "%s;%s;%s", sys32, windir, gitdir);
    else
      (void)snprintf(pathv, pn, "%s;%s", sys32, windir);
    e = strv_push(&envp, &envn, &envcap, env_kv("PATH", pathv));
    free(pathv);
    if (e == ASTOOLS_OK)
      e = strv_push(&envp, &envn, &envcap, env_kv("SystemRoot", windir));
    if (e == ASTOOLS_OK)
      e = strv_push(&envp, &envn, &envcap, env_kv("windir", windir));
    if (e == ASTOOLS_OK) {
      char drive[3];
      drive[0] = windir[0] != '\0' ? windir[0] : 'C';
      drive[1] = ':';
      drive[2] = '\0';
      e = strv_push(&envp, &envn, &envcap, env_kv("SystemDrive", drive));
    }
    if (e == ASTOOLS_OK) {
      size_t cn = strlen(sys32) + sizeof "\\cmd.exe";
      comspec = malloc(cn);
      if (!comspec) {
        e = ASTOOLS_ERR_NOMEM;
      } else {
        (void)snprintf(comspec, cn, "%s\\cmd.exe", sys32);
        e = strv_push(&envp, &envn, &envcap, env_kv("ComSpec", comspec));
        free(comspec);
      }
    }
    if (e == ASTOOLS_OK)
      e = strv_push(&envp, &envn, &envcap,
                    astools_strdup("PATHEXT=.COM;.EXE;.BAT;.CMD"));
  }
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("HOME", scratch));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("USERPROFILE", scratch));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("TMPDIR", scratch));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("TEMP", scratch));
  if (e == ASTOOLS_OK)
    e = strv_push(&envp, &envn, &envcap, env_kv("TMP", scratch));
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

  /* 4. Exec vector (no jail wrapper on Windows). */
  for (n = 0; entry_argv[n] != NULL; n++) {}
  for (i = 0; i < n; i++) {
    e = strv_push(&argv, &argn, &argcap, astools_strdup(entry_argv[i]));
    if (e != ASTOOLS_OK) {
      e = astools_seterr(c, e, "sandbox: cannot build exec vector");
      goto fail;
    }
  }

  /* 5. Limits. CPU stays 0: invoke derives it from the deadline and the
   * worker applies it at spawn. Memory and process caps are Job-Object
   * enforced per tree, so an absolute process cap is meaningful here
   * (unlike the per-uid POSIX rlimit); level none is debug mode and
   * stays unlimited. */
  out->limit_cpu_seconds = 0;
  if (level != ASTOOLS_SB_NONE) {
    out->limit_mem_bytes = (int64_t)1 << 30;
    out->limit_nproc = ASTOOLS_NPROC_HEADROOM;
  }

  out->level = level;
  out->argv = argv;
  out->envp = envp;
  out->scratch_dir = scratch;
  free(sys32);
  free(windir);
  return ASTOOLS_OK;

fail:
  free(sys32);
  free(windir);
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

/* ---- process cap ---------------------------------------------- */

astools_err astools_sandbox_nproc_cap(int64_t *out) {
  /* The per-uid task count has no Windows analogue; prepare() sets the
   * per-tree Job Object cap directly instead. */
  if (out) *out = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

/* ---- honest capability report -------------------------------- */

astools_err astools_sandbox_caps_impl(int strict, astools_sandbox_caps *out) {
  if (!out) return ASTOOLS_ERR_INVALID;
  memset(out, 0, sizeof *out);
  /* basic: every spawn runs in its own Job Object, so the cpu / memory /
   * process caps are kernel-enforced. strict adds nothing on Windows yet
   * (no fs confinement, net deny, syscall filter or privilege drop). */
  (void)strict;
  out->cpu_cap = 1;
  out->memory_cap = 1;
  out->process_cap = 1;
  return ASTOOLS_OK;
}

#else /* !_WIN32 */

typedef int astools_sandbox_win32_unused_on_posix; /* non-empty unit */

#endif /* _WIN32 */
