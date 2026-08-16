/*
 * sandbox_stub.c — honest ASTOOLS_ERR_UNSUPPORTED sandbox backend for
 * platforms without one (Windows deferred; strict_fallback still
 * happens above this layer). Capability reporting is all zeros: nothing
 * is enforced here and nothing pretends to be.
 *
 * Added to the build only where sandbox_posix.c is not, but the file
 * itself compiles on every platform; the two are never linked together.
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

astools_err astools_sandbox_prepare(astools_ctx *c, const astools_tool *t,
                                    const astools_effective *eff,
                                    const char *invocation_id,
                                    char *const *entry_argv,
                                    astools_sandbox_setup *out) {
  (void)c;
  (void)t;
  (void)eff;
  (void)invocation_id;
  (void)entry_argv;
  /* Zero the setup so a cleanup on the failure path stays a no-op. */
  if (out) memset(out, 0, sizeof(*out));
  return ASTOOLS_ERR_UNSUPPORTED;
}

void astools_sandbox_cleanup(astools_ctx *c, astools_sandbox_setup *s,
                             bool keep_scratch) {
  (void)c;
  (void)keep_scratch;
  if (!s) return;
  /* prepare() above never allocates, but free defensively so the pair
   * stays safe if a caller filled the struct through another path. */
  astools_argv_free(s->argv);
  astools_argv_free(s->envp);
  free(s->scratch_dir);
  memset(s, 0, sizeof(*s));
}

astools_err astools_sandbox_caps_impl(int strict, astools_sandbox_caps *out) {
  (void)strict;
  if (out) memset(out, 0, sizeof(*out)); /* honest: nothing enforced */
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err astools_sandbox_nproc_cap(int64_t *out) {
  /* Windows bounds processes with a Job Object, not a per-uid rlimit. */
  if (out) *out = 0;
  return ASTOOLS_ERR_UNSUPPORTED;
}

astools_err astools_entry_resolve_argv(const astools_tool *t, char ***out) {
  (void)t;
  if (out) *out = NULL;
  return ASTOOLS_ERR_UNSUPPORTED;
}

void astools_argv_free(char **argv) {
  size_t i;
  if (!argv) return;
  for (i = 0; argv[i] != NULL; i++) free(argv[i]);
  free(argv);
}
