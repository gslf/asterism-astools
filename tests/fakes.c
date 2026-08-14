/*
 * fakes.c — fixture writers for the astools integration tests (fakes.h).
 *
 * Links astools_static for astools_buf / os_* / astools_plat_* only; the
 * emitted text is ordinary #astools_tool / #astools_config source that the
 * library under test parses on its own.
 */

#include "fakes.h"

#include <stdlib.h>
#include <string.h>

#include "astools_internal.h"

const char FAKE_CMD_RUN_ECHO[] =
    "#command {\n"
    "  name: \"run\",\n"
    "  summary: \"Echo the validated arguments back.\",\n"
    "  params: [\n"
    "    #param { name: \"msg\", type: #type { kind: \"string\" },"
    " required: true },\n"
    "    #param { name: \"p\", type: #type { kind: \"path\","
    " access: \"read-write\" }, required: false },\n"
    "    #param { name: \"count\", type: #type { kind: \"integer\" },"
    " required: false, default: 7 },\n"
    "  ],\n"
    "  examples: [\n"
    "    #example { title: \"echo\", call: { msg: \"hi\" },"
    " result: { msg: \"hi\", count: 7 } },\n"
    "  ],\n"
    "}";

const char FAKE_CMD_RUN_MS[] =
    "#command {\n"
    "  name: \"run\",\n"
    "  summary: \"Sleep for ms milliseconds, then reply.\",\n"
    "  params: [\n"
    "    #param { name: \"ms\", type: #type { kind: \"integer\", min: 0 },"
    " required: true },\n"
    "  ],\n"
    "  examples: [\n"
    "    #example { title: \"nap\", call: { ms: 10 }, result: {} },\n"
    "  ],\n"
    "}";

const char FAKE_CMD_RUN_EMPTY[] =
    "#command {\n"
    "  name: \"run\",\n"
    "  summary: \"Run with no parameters.\",\n"
    "  params: [],\n"
    "  examples: [\n"
    "    #example { title: \"run\", call: {}, result: {} },\n"
    "  ],\n"
    "}";

static const char DEFAULT_PERMS[] =
    "fs: [ { path: \"${workspace}\", access: \"read-write\" } ],\n"
    "    net: false,\n"
    "    proc: false,\n"
    "    env: []";

/* Append s as the body of a double-quoted xCDN string literal. */
static astools_err append_escaped(astools_buf *b, const char *s) {
  astools_err e = ASTOOLS_OK;
  if (!s) return ASTOOLS_OK;
  for (; *s != '\0' && e == ASTOOLS_OK; s++) {
    if (*s == '"' || *s == '\\') e = astools_buf_appendc(b, '\\');
    if (e == ASTOOLS_OK) e = astools_buf_appendc(b, *s);
  }
  return e;
}

/* Write buf to path via a plain truncating write (tests only). */
static int write_text(const char *path, const astools_buf *b) {
  return os_write_file(path, b->data != NULL ? b->data : "", b->len) ==
         ASTOOLS_OK;
}

int fake_registry_write(const char *root, const char *tool_id,
                        const char *tool_bin, const char *behavior,
                        const char *permissions, const char *commands,
                        const char *extra_fields) {
  astools_buf b;
  astools_err e;
  char *pkg_dir = NULL, *manifest_path = NULL;
  int ok = 0;

  if (!root || !tool_id) return 0;
  if (!behavior) behavior = "echo";
  if (!permissions) permissions = DEFAULT_PERMS;
  if (!commands) commands = FAKE_CMD_RUN_ECHO;

  astools_buf_init(&b);
  e = astools_buf_appends(&b,
                          "#astools_tool {\n"
                          "  manifest_version: 1,\n"
                          "  id: \"");
  if (e == ASTOOLS_OK) e = append_escaped(&b, tool_id);
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&b,
                            "\",\n"
                            "  version: \"1.0.0\",\n"
                            "  title: \"Fake tool\",\n"
                            "  summary: \"Deterministic scripted tool for "
                            "the test-suite.\",\n"
                            "  kind: \"executable\",\n");
  if (e == ASTOOLS_OK)
    e = astools_buf_printf(&b, "  platforms: [\"%s\"],\n", astools_plat_os());
  if (e == ASTOOLS_OK)
    e = astools_buf_printf(&b,
                           "  runtime: {\n"
                           "    mode: \"oneshot\",\n"
                           "    entry: [\n"
                           "      { os: \"%s\", arch: \"%s\", argv: [\"",
                           astools_plat_os(), astools_plat_arch());
  if (e == ASTOOLS_OK)
    e = append_escaped(&b, tool_bin != NULL ? tool_bin : "bin/fake");
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, "\", \"");
  if (e == ASTOOLS_OK) e = append_escaped(&b, behavior);
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&b,
                            "\"] },\n"
                            "    ],\n"
                            "  },\n"
                            "  permissions: {\n"
                            "    ");
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, permissions);
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, ",\n  },\n");
  if (e == ASTOOLS_OK && extra_fields != NULL) {
    e = astools_buf_appends(&b, "  ");
    if (e == ASTOOLS_OK) e = astools_buf_appends(&b, extra_fields);
    if (e == ASTOOLS_OK) e = astools_buf_appendc(&b, '\n');
  }
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, "  commands: [\n");
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, commands);
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, ",\n  ],\n}\n");
  if (e != ASTOOLS_OK) goto done;

  pkg_dir = os_path_join(root, tool_id);
  if (!pkg_dir) goto done;
  if (os_mkdir_p(pkg_dir) != ASTOOLS_OK) goto done;
  manifest_path = os_path_join(pkg_dir, "manifest.xcdn");
  if (!manifest_path) goto done;
  ok = write_text(manifest_path, &b);

done:
  free(pkg_dir);
  free(manifest_path);
  astools_buf_free(&b);
  return ok;
}

int fake_config_write(const char *path, const char *root_dir, int trust_full,
                      const char *workspace, const char *extra) {
  astools_buf b;
  astools_err e;
  int ok = 0;

  if (!path || !root_dir || !workspace) return 0;

  astools_buf_init(&b);
  e = astools_buf_appends(&b,
                          "#astools_config {\n"
                          "  registry: {\n"
                          "    paths: [ { path: \"");
  if (e == ASTOOLS_OK) e = append_escaped(&b, root_dir);
  if (e == ASTOOLS_OK)
    e = astools_buf_printf(&b,
                           "\", trust: \"%s\" } ],\n"
                           "    watch: \"off\",\n"
                           "    pinning: \"off\",\n"
                           "  },\n"
                           "  workspace: { root: \"",
                           trust_full ? "full" : "standard");
  if (e == ASTOOLS_OK) e = append_escaped(&b, workspace);
  if (e == ASTOOLS_OK)
    e = astools_buf_appends(&b,
                            "\" },\n"
                            "  logging: { level: \"debug\" },\n");
  if (e == ASTOOLS_OK && extra != NULL) {
    e = astools_buf_appends(&b, "  ");
    if (e == ASTOOLS_OK) e = astools_buf_appends(&b, extra);
    if (e == ASTOOLS_OK) e = astools_buf_appendc(&b, '\n');
  }
  if (e == ASTOOLS_OK) e = astools_buf_appends(&b, "}\n");
  if (e != ASTOOLS_OK) goto done;

  ok = write_text(path, &b);

done:
  astools_buf_free(&b);
  return ok;
}
