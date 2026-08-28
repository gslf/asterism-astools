/*
 * tool_code.c — bounded, process-free coding primitives.
 *
 * These commands deliberately cannot execute programs.  read-range is
 * line-based (unlike fs.read's byte paging), search-symbol turns a validated
 * identifier into a whole-identifier regex before delegating to grep, and
 * apply-patch reuses edit's atomic all-or-nothing patch implementation.
 */

#include "sdk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_READ_CAP (16u * 1024u * 1024u)

static int symbol_valid(const char *s) {
  size_t i;
  unsigned char ch;
  if (!s || !s[0]) return 0;
  ch = (unsigned char)s[0];
  if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
        ch == '_' || ch == '$'))
    return 0;
  for (i = 1; s[i]; i++) {
    ch = (unsigned char)s[i];
    if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
          (ch >= '0' && ch <= '9') || ch == '_' || ch == '$'))
      return 0;
  }
  return i <= 256;
}

static void cmd_read_range(astd_req *r) {
  const char *path = astd_arg_str(r, "path", NULL);
  int64_t start = astd_arg_int(r, "start_line", 1);
  int64_t end = astd_arg_int(r, "end_line", 200);
  FILE *f = NULL;
  char *buf = NULL, *content = NULL;
  long flen;
  size_t n, i, from = 0, to = 0;
  int64_t line = 1, total = 0, actual_end;
  xcdn_value_t *res = NULL;
  int rc = 0;

  if (!path) {
    astd_fail(r, "astools/invalid-args", "path is required");
    return;
  }
  if (start < 1 || end < start || end - start + 1 > 1000) {
    astd_fail(r, "code/bad-range",
              "line range must be ascending and contain at most 1000 lines");
    return;
  }
  f = fopen(path, "rb");
  if (!f) {
    astd_fail(r, "code/not-found", "cannot open '%s'", path);
    return;
  }
  if (fseek(f, 0, SEEK_END) != 0 || (flen = ftell(f)) < 0 ||
      (unsigned long)flen > CODE_READ_CAP || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    astd_fail(r, "code/too-large",
              "read-range accepts text files up to %u bytes",
              (unsigned)CODE_READ_CAP);
    return;
  }
  n = (size_t)flen;
  buf = malloc(n + 1);
  if (!buf) goto oom;
  if (n > 0 && fread(buf, 1, n, f) != n) {
    fclose(f);
    free(buf);
    astd_fail(r, "code/io", "cannot read '%s'", path);
    return;
  }
  fclose(f);
  f = NULL;
  buf[n] = '\0';
  if (memchr(buf, '\0', n) != NULL || astd_looks_binary(buf, n)) {
    free(buf);
    astd_fail(r, "code/binary", "read-range accepts UTF-8 text files only");
    return;
  }

  total = n == 0 ? 0 : 1;
  for (i = 0; i < n; i++)
    if (buf[i] == '\n') total++;
  if (n > 0 && buf[n - 1] == '\n') total--;

  from = n;
  to = n;
  line = 1;
  for (i = 0; i < n; i++) {
    if (line == start && from == n) from = i;
    if (buf[i] == '\n') {
      if (line == end) {
        to = i + 1;
        break;
      }
      line++;
    }
  }
  if (start <= total && from == n) from = n;
  if (start > total) from = to = n;
  else if (to == n) to = n;
  content = malloc(to - from + 1);
  if (!content) goto oom;
  memcpy(content, buf + from, to - from);
  content[to - from] = '\0';
  actual_end = total < end ? total : end;
  if (actual_end < start) actual_end = start - 1;

  res = xcdn_value_object();
  if (!res) goto oom;
  rc |= astd_set_str(res, "content", content);
  rc |= astd_set_int(res, "start_line", start);
  rc |= astd_set_int(res, "end_line", actual_end);
  rc |= astd_set_int(res, "total_lines", total);
  rc |= astd_set_bool(res, "truncated", end < total);
  free(content);
  free(buf);
  if (rc != 0) {
    xcdn_value_free(res);
    astd_fail(r, "code/failed", "out of memory");
    return;
  }
  astd_ok(r, res);
  return;

oom:
  if (f) fclose(f);
  free(content);
  free(buf);
  xcdn_value_free(res);
  astd_fail(r, "code/failed", "out of memory");
}

static void cmd_search_symbol(astd_req *r) {
  const char *symbol = astd_arg_str(r, "symbol", NULL);
  const char *path = astd_arg_str(r, "path", NULL);
  const char *glob = astd_arg_str(r, "glob", NULL);
  int64_t max_results = astd_arg_int(r, "max_results", 100);
  size_t sl;
  char *pattern;
  xcdn_value_t *args;
  const xcdn_value_t *saved_args;
  const char *saved_command;
  int rc = 0;

  if (!symbol_valid(symbol)) {
    astd_fail(r, "code/bad-symbol",
              "symbol must be one identifier of at most 256 characters");
    return;
  }
  sl = strlen(symbol);
  pattern = malloc(sl + 52);
  if (!pattern) {
    astd_fail(r, "code/failed", "out of memory");
    return;
  }
  snprintf(pattern, sl + 52, "(^|[^A-Za-z0-9_$])%s([^A-Za-z0-9_$]|$)",
           symbol);
  args = xcdn_value_object();
  if (!args) {
    free(pattern);
    astd_fail(r, "code/failed", "out of memory");
    return;
  }
  rc |= astd_set_str(args, "pattern", pattern);
  rc |= astd_set_str(args, "path", path ? path : ".");
  rc |= astd_set_bool(args, "regex", 1);
  rc |= astd_set_bool(args, "case_sensitive", 1);
  rc |= astd_set_bool(args, "recursive", 1);
  rc |= astd_set_int(args, "context", 0);
  rc |= astd_set_int(args, "max_results", max_results);
  rc |= astd_set_int(args, "max_file_bytes", 1048576);
  if (glob) rc |= astd_set_str(args, "glob", glob);
  free(pattern);
  if (rc != 0) {
    xcdn_value_free(args);
    astd_fail(r, "code/failed", "out of memory");
    return;
  }

  saved_args = r->args;
  saved_command = r->command;
  r->args = args;
  r->command = "search";
  (void)astd_tool_grep(r);
  r->command = saved_command;
  r->args = saved_args;
  xcdn_value_free(args);
}

static void cmd_apply_patch(astd_req *r) {
  const char *saved = r->command;
  r->command = "patch";
  (void)astd_tool_edit(r);
  r->command = saved;
}

int astd_tool_code(astd_req *r) {
  if (strcmp(r->command, "read-range") == 0)
    cmd_read_range(r);
  else if (strcmp(r->command, "search-symbol") == 0)
    cmd_search_symbol(r);
  else if (strcmp(r->command, "apply-patch") == 0)
    cmd_apply_patch(r);
  else
    astd_fail(r, "astools/protocol", "unknown code command '%s'",
              r->command);
  return 0;
}
