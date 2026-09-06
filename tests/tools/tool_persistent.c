/* Independent, line-oriented peer for lifecycle and cancellation regressions. */
#define _POSIX_C_SOURCE 200809L
#include "xcdn.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void pause_ms(long ms) {
  struct timespec delay = {ms / 1000, (ms % 1000) * 1000000};
  while (nanosleep(&delay, &delay)) {
  }
}
static const xcdn_value_t *field(const xcdn_value_t *v, const char *key) {
  const xcdn_node_t *n = xcdn_object_get(v, key);
  return n ? n->value : NULL;
}
static void marker(const char *name) {
  char path[4096];
  snprintf(path, sizeof path, "%s/%s", getenv("ASTOOLS_WORKSPACE"), name);
  FILE *f = fopen(path, "w");
  if (f) fclose(f);
}
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "first";
  if (!strcmp(mode, "slow")) pause_ms(800);
  puts("#tool_hello {protocol:1,tool:\"pp\",version:\"1.0.0\",parallel:1}");
  fflush(stdout);
  char line[16384];
  int calls = 0;
  while (fgets(line, sizeof line, stdin)) {
    xcdn_error_t error;
    xcdn_document_t *doc = xcdn_parse(line, &error);
    if (!doc || !doc->values_len) {
      xcdn_document_free(doc);
      continue;
    }
    const xcdn_value_t *r = doc->values[0]->value;
    const xcdn_value_t *id = field(r, "invocation_id"), *args = field(r, "args");
    if (!id || !args) {
      xcdn_document_free(doc);
      continue;
    }
    const xcdn_value_t *delay = field(args, "ms"), *scratch = field(r, "scratch");
    marker("accepted");
    if (delay) pause_ms((long)delay->data.integer);
    if (!strcmp(mode, "tail") && !calls) {
      int barrier[2];
      if (pipe(barrier)) return 2;
      pid_t child = fork();
      if (child < 0) return 2;
      if (!child) {
        signal(SIGTERM, SIG_IGN);
        close(barrier[0]);
        if (write(barrier[1], "x", 1) != 1) _exit(2);
        close(barrier[1]);
        pause_ms(600);
        marker("survived");
        _exit(0);
      }
      close(barrier[1]);
      char ready;
      if (read(barrier[0], &ready, 1) != 1) return 2;
      close(barrier[0]);
    }
    char cwd[4096];
    int match = getcwd(cwd, sizeof cwd) && scratch && !strcmp(cwd, scratch->data.string) &&
                !strcmp(cwd, getenv("ASTOOLS_SCRATCH"));
    FILE *f = fopen("state", "a");
    int writable = f != NULL;
    if (f) {
      fputs("x", f);
      fclose(f);
    }
    printf("#tool_response {protocol:1,invocation_id:u\"%s\",ok:true,result:"
           "{generation:%d,calls:%d,scratch_ok:%s,scratch:\"%s\"}}\n",
           !strcmp(mode, "wrongid") ? "00000000-0000-4000-8000-000000000000" : id->data.string,
           !strcmp(mode, "second") ? 2 : 1, ++calls, match && writable ? "true" : "false",
           scratch ? scratch->data.string : "");
    fflush(stdout);
    xcdn_document_free(doc);
  }
  return 0;
}
