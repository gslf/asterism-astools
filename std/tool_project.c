/*
 * tool_project.c — semantic project actions with closed argv.
 *
 * The model chooses an action and (optionally) a known adapter; it never
 * supplies an executable, flag, environment variable or stdin payload.
 * Every argv below is a source-code constant.  This tool still requests the
 * proc capability because it launches build-system children, but granting it
 * does not grant proc.run: policy grants are scoped to the tool id.
 */

#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "compat_win32.h" /* must come after every system include */
#else
extern char **environ;
#endif

#define PROJECT_STREAM_CAP 262144
#define PROJECT_MAX_STEPS 3

typedef struct {
  char *argv[9];
} project_step;

static char *path_join(const char *dir, const char *leaf) {
  size_t dl = strlen(dir), ll = strlen(leaf);
  char *p = malloc(dl + ll + 2);
  if (!p) return NULL;
  memcpy(p, dir, dl);
  p[dl] = '/';
  memcpy(p + dl + 1, leaf, ll + 1);
  return p;
}

static int marker_exists(const char *dir, const char *leaf) {
  char *p = path_join(dir, leaf);
  FILE *f;
  if (!p) return 0;
  f = fopen(p, "rb");
  free(p);
  if (!f) return 0;
  fclose(f);
  return 1;
}

static const char *detect_adapter(const char *dir) {
  if (marker_exists(dir, "CMakeLists.txt")) return "cmake";
  if (marker_exists(dir, "Cargo.toml")) return "cargo";
  if (marker_exists(dir, "package.json")) return "npm";
  if (marker_exists(dir, "pyproject.toml") ||
      marker_exists(dir, "setup.py") || marker_exists(dir, "pytest.ini"))
    return "python";
  return NULL;
}

#ifdef _WIN32
/* The scrubbed environment intentionally does not contain a Visual Studio
 * developer prompt. Prefer CMake's MinGW generator when the operator exposed
 * both parts of that toolchain through sandbox.executable_paths; otherwise
 * retain CMake's native default generator. */
static int program_on_path(const char *leaf) {
  const char *path = getenv("PATH");
  const char *p = path;
  if (!path || !*path) return 0;
  for (;;) {
    const char *q = strchr(p, ';');
    size_t dl = q ? (size_t)(q - p) : strlen(p);
    if (dl > 0) {
      size_t ll = strlen(leaf);
      char *candidate = malloc(dl + ll + 2);
      if (!candidate) return 0;
      memcpy(candidate, p, dl);
      candidate[dl] = '/';
      memcpy(candidate + dl + 1, leaf, ll + 1);
      if (access(candidate, X_OK) == 0) {
        free(candidate);
        return 1;
      }
      free(candidate);
    }
    if (!q) break;
    p = q + 1;
  }
  return 0;
}

static int have_mingw_toolchain(void) {
  return program_on_path("gcc.exe") && program_on_path("mingw32-make.exe");
}
#endif

static size_t cmake_steps(const char *action, project_step *s) {
  s[0].argv[0] = "cmake";
  s[0].argv[1] = "-S";
  s[0].argv[2] = ".";
  s[0].argv[3] = "-B";
  s[0].argv[4] = "build";
#ifdef _WIN32
  if (have_mingw_toolchain()) {
    s[0].argv[5] = "-G";
    s[0].argv[6] = "MinGW Makefiles";
  }
#endif
  if (strcmp(action, "build") == 0 || strcmp(action, "diagnostics") == 0) {
    s[1].argv[0] = "cmake";
    s[1].argv[1] = "--build";
    s[1].argv[2] = "build";
    return 2;
  }
  if (strcmp(action, "test") == 0) {
    s[1].argv[0] = "cmake";
    s[1].argv[1] = "--build";
    s[1].argv[2] = "build";
    s[2].argv[0] = "ctest";
    s[2].argv[1] = "--test-dir";
    s[2].argv[2] = "build";
    s[2].argv[3] = "--output-on-failure";
    return 3;
  }
  s[1].argv[0] = "cmake";
  s[1].argv[1] = "--build";
  s[1].argv[2] = "build";
  s[1].argv[3] = "--target";
  s[1].argv[4] = strcmp(action, "lint") == 0 ? "lint" : "format";
  return 2;
}

static size_t cargo_steps(const char *action, project_step *s) {
  s[0].argv[0] = "cargo";
  if (strcmp(action, "build") == 0)
    s[0].argv[1] = "build";
  else if (strcmp(action, "test") == 0)
    s[0].argv[1] = "test";
  else if (strcmp(action, "lint") == 0) {
    s[0].argv[1] = "clippy";
    s[0].argv[2] = "--all-targets";
    s[0].argv[3] = "--";
    s[0].argv[4] = "-D";
    s[0].argv[5] = "warnings";
  } else if (strcmp(action, "format") == 0) {
    s[0].argv[1] = "fmt";
    s[0].argv[2] = "--all";
  } else {
    s[0].argv[1] = "check";
    s[0].argv[2] = "--all-targets";
  }
  return 1;
}

static size_t npm_steps(const char *action, project_step *s) {
  s[0].argv[0] = "npm";
  if (strcmp(action, "test") == 0) {
    s[0].argv[1] = "test";
  } else {
    s[0].argv[1] = "run";
    s[0].argv[2] = strcmp(action, "diagnostics") == 0 ? "typecheck" :
                                                        (char *)action;
    s[0].argv[3] = "--if-present";
  }
  return 1;
}

static size_t python_steps(const char *action, project_step *s) {
  s[0].argv[0] = "python";
  s[0].argv[1] = "-m";
  if (strcmp(action, "build") == 0) {
    s[0].argv[2] = "build";
  } else if (strcmp(action, "test") == 0) {
    s[0].argv[2] = "pytest";
  } else if (strcmp(action, "lint") == 0) {
    s[0].argv[2] = "ruff";
    s[0].argv[3] = "check";
    s[0].argv[4] = ".";
  } else if (strcmp(action, "format") == 0) {
    s[0].argv[2] = "ruff";
    s[0].argv[3] = "format";
    s[0].argv[4] = ".";
  } else {
    s[0].argv[2] = "compileall";
    s[0].argv[3] = "-q";
    s[0].argv[4] = ".";
  }
  return 1;
}

static void run_action(astd_req *r, const char *action) {
  const char *cwd = astd_arg_str(r, "path", NULL);
  const char *requested = astd_arg_str(r, "adapter", "auto");
  const char *adapter;
  project_step steps[PROJECT_MAX_STEPS];
  size_t nsteps = 0, i;
  xcdn_value_t *step_values = NULL, *res = NULL;
  int exit_code = 0, rc = 0;

  if (!cwd || strcmp(cwd, ".") == 0)
    cwd = r->workspace ? r->workspace : ".";
  adapter = strcmp(requested, "auto") == 0 ? detect_adapter(cwd) : requested;
  if (!adapter) {
    astd_fail(r, "project/no-adapter",
              "no supported project marker found at '%s'", cwd);
    return;
  }
  memset(steps, 0, sizeof steps);
  if (strcmp(adapter, "cmake") == 0)
    nsteps = cmake_steps(action, steps);
  else if (strcmp(adapter, "cargo") == 0)
    nsteps = cargo_steps(action, steps);
  else if (strcmp(adapter, "npm") == 0)
    nsteps = npm_steps(action, steps);
  else if (strcmp(adapter, "python") == 0)
    nsteps = python_steps(action, steps);
  else {
    astd_fail(r, "project/no-adapter", "unsupported adapter '%s'", adapter);
    return;
  }

  step_values = xcdn_value_array();
  if (!step_values) goto oom;
  for (i = 0; i < nsteps; i++) {
    astd_run_res rr;
    xcdn_value_t *sv;
    char emsg[512];
    memset(&rr, 0, sizeof rr);
    if (astd_run_capture(steps[i].argv, environ, cwd, NULL, 0,
                         PROJECT_STREAM_CAP, PROJECT_STREAM_CAP, 0, &rr,
                         emsg, sizeof emsg) != 0) {
      xcdn_value_free(step_values);
      astd_fail(r, "project/not-installed", "%s", emsg);
      return;
    }
    sv = xcdn_value_object();
    if (!sv) {
      free(rr.out);
      free(rr.err);
      goto oom;
    }
    rc = 0;
    rc |= astd_set_str(sv, "program", steps[i].argv[0]);
    rc |= astd_set_int(sv, "exit_code", rr.exit_code);
    rc |= astd_set_str(sv, "stdout", rr.out ? rr.out : "");
    rc |= astd_set_str(sv, "stderr", rr.err ? rr.err : "");
    rc |= astd_set_bool(sv, "stdout_truncated", rr.out_trunc);
    rc |= astd_set_bool(sv, "stderr_truncated", rr.err_trunc);
    exit_code = rr.exit_code;
    free(rr.out);
    free(rr.err);
    if (rc != 0) {
      xcdn_value_free(sv);
      goto oom;
    }
    if (astd_arr_push_val(step_values, sv) != 0) goto oom;
    if (exit_code != 0) break;
  }

  res = xcdn_value_object();
  if (!res) goto oom;
  if (astd_set_str(res, "adapter", adapter) != 0 ||
      astd_set_int(res, "exit_code", exit_code) != 0) {
    xcdn_value_free(step_values);
    xcdn_value_free(res);
    astd_fail(r, "project/failed", "out of memory");
    return;
  }
  if (astd_set_val(res, "steps", step_values) != 0) {
    /* astd_set_val consumes step_values on both success and failure. */
    xcdn_value_free(res);
    astd_fail(r, "project/failed", "out of memory");
    return;
  }
  astd_ok(r, res);
  return;

oom:
  xcdn_value_free(step_values);
  xcdn_value_free(res);
  astd_fail(r, "project/failed", "out of memory");
}

int astd_tool_project(astd_req *r) {
  if (strcmp(r->command, "build") == 0 ||
      strcmp(r->command, "test") == 0 ||
      strcmp(r->command, "lint") == 0 ||
      strcmp(r->command, "format") == 0 ||
      strcmp(r->command, "diagnostics") == 0)
    run_action(r, r->command);
  else
    astd_fail(r, "astools/protocol", "unknown project command '%s'",
              r->command);
  return 0;
}
