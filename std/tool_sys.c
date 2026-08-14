/*
 * tool_sys.c — the `sys` standard tool (§8.7): sys.info reports host
 * facts (os, arch, hostname, cpus, memory, astools version). Read-only,
 * no permissions required.
 */

#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#endif

#include "sdk.h"
#ifdef _WIN32

int astd_tool_sys(astd_req *r) {
  astd_fail(r, "std/unsupported", "sys is not implemented on this platform");
  return 0;
}

#else /* POSIX */

#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined(__linux__)
#include <sys/sysinfo.h>
#endif

static const char *sys_os(void) {
#if defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

static const char *sys_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
  return "x86_64";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
  return "x86";
#elif defined(__arm__)
  return "arm";
#elif defined(__riscv)
  return "riscv";
#else
  return "unknown";
#endif
}

static int64_t sys_memory_mb(void) {
#if defined(__APPLE__)
  uint64_t mem = 0;
  size_t len = sizeof mem;
  if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) == 0)
    return (int64_t)(mem / (1024u * 1024u));
  return 0;
#elif defined(__linux__)
  struct sysinfo si;
  if (sysinfo(&si) == 0)
    return (int64_t)(((uint64_t)si.totalram * si.mem_unit) /
                     (1024u * 1024u));
  return 0;
#else
  return 0; /* unknown */
#endif
}

static int64_t sys_cpus(void) {
#ifdef _SC_NPROCESSORS_ONLN
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n > 0) return (int64_t)n;
#endif
  return 0; /* unknown */
}

int astd_tool_sys(astd_req *r) {
  char host[256];
  xcdn_value_t *res;

  if (!r->command || strcmp(r->command, "info") != 0) {
    astd_fail(r, "astools/invalid-args", "unknown sys command \"%s\"",
              r->command ? r->command : "");
    return 0;
  }

  if (gethostname(host, sizeof host) != 0)
    memcpy(host, "unknown", sizeof "unknown");
  host[sizeof host - 1] = '\0'; /* gethostname may not terminate */

  res = xcdn_value_object();
  if (!res) goto oom;
  if (astd_set_str(res, "os", sys_os()) != 0 ||
      astd_set_str(res, "arch", sys_arch()) != 0 ||
      astd_set_str(res, "hostname", host) != 0 ||
      astd_set_int(res, "cpus", sys_cpus()) != 0 ||
      astd_set_int(res, "memory_total_mb", sys_memory_mb()) != 0 ||
      astd_set_str(res, "astools_version", "0.3.0") != 0) {
    xcdn_value_free(res);
    goto oom;
  }
  astd_ok(r, res);
  return 0;

oom:
  astd_fail(r, "astools/protocol", "out of memory");
  return 0;
}

#endif /* _WIN32 */
