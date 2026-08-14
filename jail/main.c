/*
 * astools-jail — standalone Seatbelt confinement helper (§6, §14).
 *
 * Usage: astools-jail -- <tool argv...>
 *
 * The parent (libastools) generates an SBPL profile and passes it in the
 * environment variable ASTOOLS_JAIL_PROFILE; this helper applies it with
 * sandbox_init(3) (deprecated since 10.8 but functional, §14), scrubs the
 * variable, and execs the tool with the sandbox already in force. The tool
 * argv[0] is resolved by the parent to an absolute path (no PATH search
 * here). Standalone on purpose: plain C99 + unistd, no libastools.
 *
 * Exit codes (chosen above the usual tool range so the runtime can tell
 * jail failures from tool exits):
 *   2    usage error (no "--" separator or no tool argv)
 *   124  ASTOOLS_JAIL_PROFILE missing on macOS — confinement was requested,
 *        refuse to run the tool unconfined
 *   125  sandbox_init failed
 *   127  exec failed
 *
 * Non-Apple builds are a documented passthrough (unsetenv + execv): the
 * library only invokes the jail on macOS strict; building it elsewhere just
 * keeps the target portable.
 */

#if !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sandbox.h>
#endif

int main(int argc, char **argv) {
  int i, sep = -1;
  char **rest;
  const char *profile;

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--") == 0) {
      sep = i;
      break;
    }
  }
  if (sep < 0 || sep + 1 >= argc) {
    fprintf(stderr, "usage: astools-jail -- <tool argv...>\n");
    return 2;
  }
  rest = &argv[sep + 1];

  profile = getenv("ASTOOLS_JAIL_PROFILE");

#if defined(__APPLE__)
  if (profile == NULL || profile[0] == '\0') {
    /* Being exec'd at all means confinement was requested; running the
     * tool without a profile would silently drop the sandbox. */
    fprintf(stderr,
            "astools-jail: ASTOOLS_JAIL_PROFILE is not set; refusing to "
            "run unconfined\n");
    _exit(124);
  }
  {
    char *errbuf = NULL;
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (sandbox_init(profile, 0, &errbuf) != 0) {
      fprintf(stderr, "astools-jail: sandbox_init failed: %s\n",
              errbuf != NULL ? errbuf : "unknown error");
      if (errbuf != NULL) sandbox_free_error(errbuf);
      _exit(125);
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
  }
#else
  (void)profile; /* passthrough: the library never invokes us here */
#endif

  unsetenv("ASTOOLS_JAIL_PROFILE");
  execv(rest[0], rest);
  perror("astools-jail: execv");
  _exit(127);
}
