/*
 * astools-jail — standalone Seatbelt confinement helper.
 *
 * Usage: astools-jail -- <tool argv...>
 *
 * The parent (libastools) generates an SBPL profile and passes it in the
 * environment variable ASTOOLS_JAIL_PROFILE; this helper applies it with
 * sandbox_init(3) (deprecated since 10.8 but functional), scrubs the
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
 * Linux strict uses this helper too: Landlock confines filesystem access and
 * seccomp denies network syscalls when the invocation has no net grant.
 */

#if defined(__linux__)
#define _GNU_SOURCE 1
#elif !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sandbox.h>
#elif defined(__linux__)
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#endif

#if defined(__linux__)

static __u64 landlock_rights(int abi) {
  __u64 r = LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
            LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
            LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
            LANDLOCK_ACCESS_FS_MAKE_SYM;
#ifdef LANDLOCK_ACCESS_FS_REFER
  if (abi >= 2) r |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
  if (abi >= 3) r |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
#ifdef LANDLOCK_ACCESS_FS_IOCTL_DEV
  if (abi >= 5) r |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
#endif
  return r;
}

static int add_path_rule(int ruleset_fd, const char *spec, __u64 handled) {
  struct landlock_path_beneath_attr rule;
  struct stat st;
  __u64 read_rights = LANDLOCK_ACCESS_FS_EXECUTE |
                      LANDLOCK_ACCESS_FS_READ_FILE |
                      LANDLOCK_ACCESS_FS_READ_DIR;
  int fd, writable;
  if (!spec || spec[0] == '\0' || spec[1] != ':') return -1;
  writable = spec[0] == 'W';
  if (!writable && spec[0] != 'R') return -1;
  if (stat(spec + 2, &st) != 0) return errno == ENOENT ? 0 : -1;
  fd = open(spec + 2, O_PATH | O_CLOEXEC);
  if (fd < 0) return -1;
  memset(&rule, 0, sizeof rule);
  rule.parent_fd = fd;
  if (S_ISDIR(st.st_mode)) {
    rule.allowed_access = writable ? handled : read_rights;
  } else {
    rule.allowed_access = read_rights & ~LANDLOCK_ACCESS_FS_READ_DIR;
    if (writable) {
      rule.allowed_access |= LANDLOCK_ACCESS_FS_WRITE_FILE;
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
      rule.allowed_access |= handled & LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    }
  }
  rule.allowed_access &= handled;
  if (syscall(__NR_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
              &rule, 0) != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

static int apply_landlock(void) {
  struct landlock_ruleset_attr attr;
  const char *count_text = getenv("ASTOOLS_JAIL_RULES");
  char *end = NULL;
  long count, i;
  int abi, fd;
  if (!count_text) return -1;
  count = strtol(count_text, &end, 10);
  if (!end || *end != '\0' || count < 1 || count > 4096) return -1;
  abi = (int)syscall(__NR_landlock_create_ruleset, NULL, 0,
                     LANDLOCK_CREATE_RULESET_VERSION);
  if (abi < 1) return -1;
  memset(&attr, 0, sizeof attr);
  attr.handled_access_fs = landlock_rights(abi);
  fd = (int)syscall(__NR_landlock_create_ruleset, &attr, sizeof attr, 0);
  if (fd < 0) return -1;
  for (i = 0; i < count; i++) {
    char name[64];
    const char *spec;
    if (snprintf(name, sizeof name, "ASTOOLS_JAIL_RULE_%ld", i) < 0) {
      close(fd);
      return -1;
    }
    spec = getenv(name);
    if (add_path_rule(fd, spec, attr.handled_access_fs) != 0) {
      close(fd);
      return -1;
    }
  }
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
      syscall(__NR_landlock_restrict_self, fd, 0) != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return 0;
}

#define DENY_SYSCALL(nr)                                                    \
  BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1),                         \
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & 0xffffU))

static int apply_network_filter(void) {
  static const struct sock_filter code[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
               (unsigned)offsetof(struct seccomp_data, nr)),
#ifdef __NR_socket
      DENY_SYSCALL(__NR_socket),
#endif
#ifdef __NR_socketpair
      DENY_SYSCALL(__NR_socketpair),
#endif
#ifdef __NR_connect
      DENY_SYSCALL(__NR_connect),
#endif
#ifdef __NR_bind
      DENY_SYSCALL(__NR_bind),
#endif
#ifdef __NR_listen
      DENY_SYSCALL(__NR_listen),
#endif
#ifdef __NR_accept
      DENY_SYSCALL(__NR_accept),
#endif
#ifdef __NR_accept4
      DENY_SYSCALL(__NR_accept4),
#endif
#ifdef __NR_sendto
      DENY_SYSCALL(__NR_sendto),
#endif
#ifdef __NR_sendmsg
      DENY_SYSCALL(__NR_sendmsg),
#endif
#ifdef __NR_recvfrom
      DENY_SYSCALL(__NR_recvfrom),
#endif
#ifdef __NR_recvmsg
      DENY_SYSCALL(__NR_recvmsg),
#endif
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
  };
  struct sock_fprog prog;
  prog.len = (unsigned short)(sizeof code / sizeof code[0]);
  prog.filter = (struct sock_filter *)code;
  if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return -1;
  return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog);
}

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
  (void)profile;
#if defined(__linux__)
  if (apply_landlock() != 0) {
    perror("astools-jail: Landlock setup");
    _exit(125);
  }
  if (getenv("ASTOOLS_JAIL_NET") == NULL && apply_network_filter() != 0) {
    perror("astools-jail: seccomp setup");
    _exit(125);
  }
#endif
#endif

  unsetenv("ASTOOLS_JAIL_PROFILE");
#if defined(__linux__)
  unsetenv("ASTOOLS_JAIL_RULES");
  unsetenv("ASTOOLS_JAIL_NET");
#endif
  execv(rest[0], rest);
  perror("astools-jail: execv");
  _exit(127);
}
