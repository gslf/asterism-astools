/* Independent blocking peer for hostile LSP and lifecycle contract tests. */
#define _POSIX_C_SOURCE 200809L
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static jx_value *read_message(void) {
  char line[512];
  size_t size = 0;
  while (fgets(line, sizeof line, stdin)) {
    if (!strcmp(line, "\r\n")) break;
    if (sscanf(line, "Content-Length: %zu", &size) != 1) return NULL;
  }
  if (!size || size > 2097152) return NULL;
  char *text = malloc(size);
  jx_value *v = NULL;
  if (text && fread(text, 1, size, stdin) == size) (void)jx_parse(text, size, &v);
  free(text);
  return v;
}
static void send_message(jx_value *v) {
  char *text = jx_write(v, 0);
  jx_free(v);
  if (!text) exit(2);
  printf("Content-Length: %zu\r\n\r\n%s", strlen(text), text);
  fflush(stdout);
  free(text);
}
static jx_value *json(const char *text) {
  jx_value *v = NULL;
  (void)jx_parse(text, strlen(text), &v);
  return v;
}
static void reply(long long id, jx_value *result) {
  jx_value *v = jx_object();
  jx_object_set(v, "jsonrpc", jx_string("2.0"));
  jx_object_set(v, "id", jx_int(id));
  jx_object_set(v, "result", result);
  send_message(v);
}
static void diagnostics(const char *uri, long long version, int current) {
  jx_value *v =
      json("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{}}");
  jx_value *p = jx_object_get(v, "params");
  jx_object_set(p, "uri", jx_string(uri));
  jx_object_set(p, "version", jx_int(version));
  jx_object_set(p, "diagnostics",
                current ? json("[{\"message\":\"current "
                               "error\",\"severity\":1,\"range\":{\"start\":{\"line\":0,"
                               "\"character\":0},\"end\":{\"line\":0,\"character\":1}}}]")
                        : jx_array());
  send_message(v);
}
int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "normal";
  char *uri = NULL;
  for (;;) {
    jx_value *message = read_message();
    if (!message) break;
    const char *method = jx_string_value(jx_object_get(message, "method"));
    long long id = jx_int_value(jx_object_get(message, "id"));
    jx_value *params = jx_object_get(message, "params");
    if (method && !strcmp(method, "initialize")) {
      jx_value *result = json(
          "{\"capabilities\":{\"positionEncoding\":\"utf-8\",\"textDocumentSync\":{\"openClose\":"
          "true},\"documentSymbolProvider\":true,\"definitionProvider\":true,"
          "\"referencesProvider\":true},\"serverInfo\":{\"name\":\"fixture\",\"version\":\"1\"}}");
      if (!strcmp(mode, "utf16"))
        jx_object_set(jx_object_get(result, "capabilities"), "positionEncoding",
                      jx_string("utf-16"));
      reply(id, result);
    } else if (method && !strcmp(method, "textDocument/didOpen")) {
      jx_value *doc = jx_object_get(params, "textDocument");
      free(uri);
      uri = strdup(jx_string_value(jx_object_get(doc, "uri")));
      if (!strcmp(mode, "diagnostics") || !strcmp(mode, "stale")) {
        long long version = jx_int_value(jx_object_get(doc, "version"));
        diagnostics(uri, version - 1, 0);
        if (!strcmp(mode, "stale")) diagnostics(uri, version - 2, 0);
        else diagnostics(uri, version, 1);
      }
    } else if (method && (!strcmp(method, "textDocument/documentSymbol") ||
                          !strcmp(method, "textDocument/definition"))) {
      if (!strcmp(mode, "hang")) {
        /* stderr is drained; the request marker is observed independently. */
        int first = 1;
        char *path = uri ? strdup(uri + 7) : NULL;
        if (path) {
          char *slash = strrchr(path, '/');
          if (slash) strcpy(slash + 1, "accepted");
          first = access(path, F_OK) != 0;
          FILE *f = fopen(path, "w");
          if (f) fclose(f);
          free(path);
        }
        while (first) {
          fputs("waiting\n", stderr);
          fflush(stderr);
          struct timespec ts = {0, 10000000};
          nanosleep(&ts, NULL);
        }
      }
      if (!strcmp(mode, "change")) {
        char *path = uri ? strdup(uri + 7) : NULL;
        if (path) {
          char *slash = strrchr(path, '/');
          if (slash) strcpy(slash + 1, "épreuve.cpp");
          FILE *f = fopen(path, "w");
          if (f) {
            fputs("int changed;\n", f);
            fclose(f);
          }
          free(path);
        }
      }
      if (!strcmp(mode, "error")) {
        jx_value *v = json("{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32801}}");
        char detail[1600] = "";
        for (int i = 0; i < 200; i++) strcat(detail, "è😀");
        jx_object_set(v, "id", jx_int(id));
        jx_object_set(jx_object_get(v, "error"), "message", jx_string(detail));
        send_message(v);
      } else if (!strcmp(mode, "duplicate")) {
        const char *text = "{\"jsonrpc\":\"2.0\",\"id\":3,\"id\":3,\"result\":[]}";
        printf("Content-Length: %zu\r\n\r\n%s", strlen(text), text);
        fflush(stdout);
      } else if (!strcmp(mode, "request")) {
        send_message(json("{\"jsonrpc\":\"2.0\",\"id\":\"host-edit\",\"method\":\"workspace/"
                          "applyEdit\",\"params\":{}}"));
        jx_value *denial = read_message();
        int refused = jx_int_value(jx_object_get(jx_object_get(denial, "error"), "code")) == -32601;
        jx_free(denial);
        if (!refused) return 3;
        reply(id, jx_array());
      } else if (!strcmp(mode, "outside"))
        reply(id, json("[{\"uri\":\"file:///etc/"
                       "passwd\",\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{"
                       "\"line\":0,\"character\":1}}}]"));
      else reply(id + (!strcmp(mode, "wrongid") ? 1 : 0), jx_array());
    }
    jx_free(message);
  }
  free(uri);
  return 0;
}
