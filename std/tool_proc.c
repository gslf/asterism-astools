/* proc.run: direct argv execution with explicit status and bounded streams.
 * The runtime owns the invocation process group, including residual descendants. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "sdk.h"
#include "duration.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include "compat_win32.h"
#else
#include <signal.h>
extern char **environ;
#endif
#define PROC_STREAM_CAP 262144


/* Inherited environment plus overrides from the env arg (map<string>).
 * Every entry is duplicated so free_env can free uniformly. */
static char **build_env(const xcdn_value_t *envmap, int *bad) {
  size_t base_n = 0, add_n = 0, i, o = 0;
  char **out;
  *bad = 0;
  while (environ[base_n]) base_n++;
  if (envmap && envmap->type == XCDN_VAL_OBJECT)
    add_n = xcdn_object_len(envmap);
  out = calloc(base_n + add_n + 1, sizeof *out);
  if (!out) return NULL;
  for (i = 0; i < base_n; i++) {
    const char *e = environ[i];
    const char *eq = strchr(e, '=');
    size_t klen = eq ? (size_t)(eq - e) : strlen(e);
    int overridden = 0;
    size_t j;
    for (j = 0; j < add_n; j++) {
      const char *k = xcdn_object_key_at(envmap, j);
      if (k && strlen(k) == klen && strncmp(k, e, klen) == 0) {
        overridden = 1;
        break;
      }
    }
    if (overridden) continue;
    out[o] = malloc(strlen(e) + 1);
    if (!out[o]) goto fail;
    memcpy(out[o], e, strlen(e) + 1);
    o++;
  }
  for (i = 0; i < add_n; i++) {
    const char *k = xcdn_object_key_at(envmap, i);
    xcdn_node_t *nd = xcdn_object_node_at(envmap, i);
    const char *v = nd && nd->value ? xcdn_value_as_string(nd->value) : NULL;
    size_t kl, vl;
    if (!k || !v) {
      *bad = 1;
      goto fail;
    }
    kl = strlen(k);
    vl = strlen(v);
    out[o] = malloc(kl + vl + 2);
    if (!out[o]) goto fail;
    memcpy(out[o], k, kl);
    out[o][kl] = '=';
    memcpy(out[o] + kl + 1, v, vl + 1);
    o++;
  }
  out[o] = NULL;
  return out;
fail:
  for (i = 0; out[i]; i++) free(out[i]);
  free(out);
  return NULL;
}

static void free_env(char **e) {
  size_t i;
  if (!e) return;
  for (i = 0; e[i]; i++) free(e[i]);
  free(e);
}

int astd_tool_proc(astd_req *r) {
  const xcdn_value_t *av, *envmap, *tv;
  const char *cwd, *input;
  char **argv = NULL, **envp = NULL;
  size_t n, i;
  int64_t timeout_ms = 0, t0, elapsed;
  int bad_env = 0;
  astd_run_res rr;
  char emsg[512];
  if (strcmp(r->command, "run") != 0) {
    astd_fail(r, "astools/protocol", "unknown proc command '%s'", r->command);
    return 0;
  }
  av = astd_arg(r, "argv");
  if (!av || av->type != XCDN_VAL_ARRAY || (n = xcdn_array_len(av)) < 1) {
    astd_fail(r, "astools/invalid-args",
              "argv must be a non-empty array of strings");
    return 0;
  }
  argv = calloc(n + 1, sizeof *argv);
  if (!argv) {
    astd_fail(r, "proc/failed", "out of memory");
    return 0;
  }
  for (i = 0; i < n; i++) {
    xcdn_node_t *nd = xcdn_array_get(av, i);
    const char *s = nd && nd->value ? xcdn_value_as_string(nd->value) : NULL;
    if (!s) {
      free(argv);
      astd_fail(r, "astools/invalid-args", "argv[%lu] is not a string",
                (unsigned long)i);
      return 0;
    }
    argv[i] = (char *)s; /* borrowed from the request document */
  }
  cwd = astd_arg_str(r, "cwd", NULL);
  if (!cwd) cwd = r->scratch ? r->scratch : r->workspace;
  input = astd_arg_str(r, "stdin", NULL);
  envmap = astd_arg(r, "env");
  tv = astd_arg(r, "timeout");
  if (tv) {
    const char *ts = xcdn_value_as_string(tv);
    if (!ts || !astools_duration_parse_ms(ts, &timeout_ms) || timeout_ms > ASTOOLS_PERIOD_MAX_MS) {
      free(argv);
      astd_fail(r, "astools/invalid-args",
                "timeout must be a fixed duration with millisecond precision, at most 4294967294 ms");
      return 0;
    }
  }
  envp = build_env(envmap, &bad_env);
  if (!envp) {
    free(argv);
    astd_fail(r, bad_env ? "astools/invalid-args" : "proc/failed",
              bad_env ? "env values must be strings" : "out of memory");
    return 0;
  }
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);
#endif
  t0 = astd_run_clock_ms();
  if (astd_run_capture(argv, envp, cwd, input, input ? strlen(input) : 0,
                       PROC_STREAM_CAP, PROC_STREAM_CAP, timeout_ms, &rr,
                       emsg, sizeof emsg) != 0) {
    free(argv);
    free_env(envp);
    astd_fail(r, "proc/failed", "%s", emsg);
    return 0;
  }
  elapsed = astd_run_clock_ms() - t0;
  if (elapsed < 0) elapsed = 0;
  free(argv);
  free_env(envp);
  {
    xcdn_value_t *res = xcdn_value_object();
    char dur[48];
    int rc = 0;
    snprintf(dur, sizeof dur, "PT%lld.%03dS", (long long)(elapsed / 1000),
             (int)(elapsed % 1000));
    if (!res) {
      free(rr.out);
      free(rr.err);
      astd_fail(r, "proc/failed", "out of memory");
      return 0;
    }
    rc |= astd_set_int(res, "exit_code", (int64_t)rr.exit_code);
    rc |= astd_set_bool(res, "timed_out", rr.timed_out);
    rc |= astd_run_output(res, &rr);
    {
      xcdn_value_t *dv = xcdn_value_duration(dur);
      if (!dv) {
        rc = -1;
      } else if (astd_set_val(res, "duration", dv) != 0) {
        xcdn_value_free(dv);
        rc = -1;
      }
    }
    free(rr.out);
    free(rr.err);
    if (rc != 0) {
      xcdn_value_free(res);
      astd_fail(r, "proc/failed", "out of memory");
      return 0;
    }
    astd_ok(r, res);
  }
  return 0;
}
