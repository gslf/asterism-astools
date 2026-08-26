/*
 * main.c — entry point of the astools-std busybox binary.
 *
 * Dispatches on `--tool <id>` (fallback: ASTOOLS_STD_TOOL) to one of the
 * astd_tool_* entry points. stdout carries exactly one #tool_response;
 * every human-facing byte (--help, --version, usage errors) goes to
 * stderr.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "sdk.h"
typedef struct {
  const char *id;
  int (*fn)(astd_req *r);
} astd_tool_entry;

static const astd_tool_entry k_tools[] = {
  { "fs", astd_tool_fs },
  { "grep", astd_tool_grep },
  { "edit", astd_tool_edit },
  { "sys", astd_tool_sys },
  { "env", astd_tool_env },
  { "proc", astd_tool_proc },
  { "git", astd_tool_git },
#ifdef ASTOOLS_BUILD_NET
  { "net", astd_tool_net },
#endif
};

static void usage(void) {
  fputs("usage: astools-std --tool <id>\n"
        "  Reads one #tool_request from stdin and writes one\n"
        "  #tool_response to stdout (astools wire protocol v1).\n"
        "  --tool <id>   tool to run (fallback: ASTOOLS_STD_TOOL)\n"
        "  --version     print the version to stderr and exit\n"
        "  --help        print this help to stderr and exit\n",
        stderr);
}

int main(int argc, char **argv) {
  const char *tool_id = NULL;
  astd_req req;
  int (*fn)(astd_req *r) = NULL;
  size_t t;
  int i, rc;

#ifdef _WIN32
  /* the wire protocol is byte-exact: no CRLF translation, and no
   * ctrl-Z end-of-input on the request */
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--tool") == 0) {
      if (i + 1 >= argc) {
        fputs("astools-std: --tool requires an argument\n", stderr);
        usage();
        return 2;
      }
      tool_id = argv[++i];
    } else if (strcmp(argv[i], "--version") == 0) {
      fputs("astools-std 0.3.0\n", stderr);
      return 0;
    } else if (strcmp(argv[i], "--help") == 0) {
      usage();
      return 0;
    } else {
      fprintf(stderr, "astools-std: unknown option \"%s\"\n", argv[i]);
      usage();
      return 2;
    }
  }
  if (!tool_id) tool_id = getenv("ASTOOLS_STD_TOOL");
  if (tool_id && tool_id[0] == '\0') tool_id = NULL;

  rc = astd_req_read(&req);
  if (rc != 0) return rc; /* error response already emitted */

  if (tool_id && req.tool && strcmp(tool_id, req.tool) != 0) {
    astd_fail(&req, "astools/protocol",
              "request tool \"%s\" does not match --tool \"%s\"", req.tool,
              tool_id);
    astd_req_free(&req);
    return 0;
  }
  if (!tool_id) tool_id = req.tool;

  for (t = 0; t < sizeof k_tools / sizeof k_tools[0]; t++) {
    if (strcmp(k_tools[t].id, tool_id) == 0) {
      fn = k_tools[t].fn;
      break;
    }
  }
  if (!fn) {
    astd_fail(&req, "astools/protocol", "unknown tool \"%s\"", tool_id);
    astd_req_free(&req);
    return 0;
  }

  rc = fn(&req);
  astd_req_free(&req);
  return rc;
}
