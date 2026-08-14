/*
 * test_manifest.c — manifest.c: #astools_tool parsing, schema checks, lint
 * (§3.2–§3.3, §17). The fixture mirrors the Appendix-A / packages/fs shape.
 */

#include "astools_test.h"

#include "astools_internal.h"

static const char FS_MANIFEST[] =
    "#astools_tool {\n"
    "  manifest_version: 1,\n"
    "  id: \"fs\",\n"
    "  version: \"1.2.0\",\n"
    "  title: \"Filesystem\",\n"
    "  summary: \"Files in the workspace.\",\n"
    "  description: \"Read and write files inside the workspace.\",\n"
    "  kind: \"executable\",\n"
    "  platforms: [\"linux\", \"macos\", \"windows\"],\n"
    "  runtime: {\n"
    "    mode: \"oneshot\",\n"
    "    entry: [\n"
    "      { os: \"linux\", arch: \"x86_64\","
    " argv: [\"bin/linux-x86_64/astools-std\", \"--tool\", \"fs\"] },\n"
    "      { os: \"macos\", arch: \"arm64\","
    " argv: [\"bin/macos-arm64/astools-std\", \"--tool\", \"fs\"] },\n"
    "      { os: \"windows\", arch: \"x86_64\","
    " argv: [\"bin/windows-x86_64/astools-std.exe\", \"--tool\", \"fs\"] },\n"
    "    ],\n"
    "  },\n"
    "  permissions: {\n"
    "    fs: [ { path: \"${workspace}\", access: \"read-write\" } ],\n"
    "    net: false, proc: false, env: [],\n"
    "  },\n"
    "  commands: [\n"
    "    #command {\n"
    "      name: \"read\",\n"
    "      summary: \"Read a file.\",\n"
    "      annotations: { read_only: true, idempotent: true },\n"
    "      timeout: r\"PT10S\",\n"
    "      params: [\n"
    "        #param { name: \"path\", type: #type { kind: \"path\","
    " access: \"read\", must_exist: true }, required: true,"
    " description: \"File to read.\" },\n"
    "        #param { name: \"offset\", type: #type { kind: \"integer\","
    " min: 0 }, required: false, default: 0 },\n"
    "        #param { name: \"max_bytes\", type: #type { kind: \"integer\","
    " min: 1, max: 1048576 }, required: false, default: 65536 },\n"
    "        #param { name: \"encoding\", type: #type { kind: \"enum\","
    " values: [\"utf8\", \"base64\"] }, required: false,"
    " default: \"utf8\" },\n"
    "      ],\n"
    "      returns: { type: #type { kind: \"object\", fields: [\n"
    "        #param { name: \"content\", type: #type { kind: \"string\" },"
    " required: true },\n"
    "        #param { name: \"size\", type: #type { kind: \"integer\" },"
    " required: true },\n"
    "        #param { name: \"truncated\", type: #type { kind: \"boolean\" },"
    " required: true },\n"
    "      ] }, description: \"File content.\" },\n"
    "      errors: [ { code: \"fs/not-found\","
    " description: \"No such file.\" } ],\n"
    "      examples: [ #example { title: \"Read a note\","
    " call: { path: \"notes/todo.txt\" },"
    " result: { content: \"x\", size: 1, truncated: false } } ],\n"
    "    },\n"
    "    #command {\n"
    "      name: \"write\",\n"
    "      summary: \"Write a file.\",\n"
    "      annotations: { destructive: true },\n"
    "      params: [\n"
    "        #param { name: \"path\", type: #type { kind: \"path\","
    " access: \"write\" }, required: true },\n"
    "        #param { name: \"content\", type: #type { kind: \"string\" },"
    " required: true },\n"
    "      ],\n"
    "      examples: [ #example { call: { path: \"a.txt\","
    " content: \"hi\" }, result: { bytes_written: 2 } } ],\n"
    "    },\n"
    "  ],\n"
    "}\n";

static astools_manifest *parse_ok(const char *text, const char *workspace) {
  char *err = NULL;
  astools_manifest *m =
      astools_manifest_parse(text, strlen(text), workspace, &err);
  if (!m) {
    ASTOOLS_FAILF("manifest rejected: %s", err ? err : "(no message)");
    free(err);
    return NULL;
  }
  free(err);
  return m;
}

/* Parse with one line of the fixture swapped out; expects rejection. */
static void expect_reject(const char *find, const char *replace) {
  char *text, *err = NULL;
  const char *pos = strstr(FS_MANIFEST, find);
  size_t before, flen, rlen, tail;
  astools_manifest *m;
  if (!pos) {
    ASTOOLS_FAILF("fixture does not contain '%s'", find);
    return;
  }
  before = (size_t)(pos - FS_MANIFEST);
  flen = strlen(find);
  rlen = strlen(replace);
  tail = strlen(pos + flen);
  text = malloc(before + rlen + tail + 1);
  if (!text) {
    ASTOOLS_FAILF("out of memory");
    return;
  }
  memcpy(text, FS_MANIFEST, before);
  memcpy(text + before, replace, rlen);
  memcpy(text + before + rlen, pos + flen, tail + 1);
  m = astools_manifest_parse(text, strlen(text), "/ws", &err);
  if (m != NULL) {
    ASTOOLS_FAILF("accepted manifest with '%s' -> '%s'", find, replace);
    astools_manifest_free(m);
  } else if (err == NULL) {
    ASTOOLS_FAILF("rejected without an error message ('%s')", replace);
  }
  free(err);
  free(text);
}

TEST(parse_fs_manifest_fields) {
  astools_manifest *m = parse_ok(FS_MANIFEST, "/ws");
  const astools_cmd *read, *write;
  if (!m) return;

  ASSERT_EQ_INT(m->manifest_version, 1);
  ASSERT_EQ_STR(m->id, "fs");
  ASSERT_EQ_STR(m->version, "1.2.0");
  ASSERT_EQ_STR(m->title, "Filesystem");
  ASSERT_EQ_STR(m->summary, "Files in the workspace.");
  ASSERT_TRUE(m->description != NULL);
  ASSERT_EQ_INT(m->kind, ASTOOLS_KIND_EXECUTABLE);
  ASSERT_EQ_INT(m->platforms_len, 3);
  ASSERT_EQ_INT(m->mode, ASTOOLS_MODE_ONESHOT);
  ASSERT_EQ_INT(m->entries_len, 3);
  ASSERT_EQ_STR(m->entries[0].os, "linux");
  ASSERT_EQ_STR(m->entries[0].arch, "x86_64");
  ASSERT_EQ_INT(m->entries[0].argv_len, 3);
  ASSERT_EQ_STR(m->entries[0].argv[0], "bin/linux-x86_64/astools-std");
  ASSERT_EQ_STR(m->entries[0].argv[2], "fs");

  /* ${workspace} expanded at load time (§3.2) */
  ASSERT_EQ_INT(m->perms.fs_len, 1);
  ASSERT_EQ_STR(m->perms.fs[0].path, "/ws");
  ASSERT_EQ_INT(m->perms.fs[0].access, ASTOOLS_ACCESS_RW);
  ASSERT_TRUE(!m->perms.net);
  ASSERT_TRUE(!m->perms.proc);
  ASSERT_EQ_INT(m->perms.env_len, 0);

  ASSERT_EQ_INT(m->commands_len, 2);
  read = astools_manifest_cmd(m, "read");
  write = astools_manifest_cmd(m, "write");
  ASSERT_TRUE(read != NULL);
  ASSERT_TRUE(write != NULL);
  ASSERT_TRUE(astools_manifest_cmd(m, "nope") == NULL);

  ASSERT_EQ_STR(read->summary, "Read a file.");
  ASSERT_TRUE(read->read_only);
  ASSERT_TRUE(read->idempotent);
  ASSERT_TRUE(!read->destructive);
  ASSERT_TRUE(!read->deprecated);
  ASSERT_EQ_INT(read->timeout_ms, 10000);
  ASSERT_EQ_INT(read->params_len, 4);
  ASSERT_EQ_STR(read->params[0].name, "path");
  ASSERT_TRUE(read->params[0].required);
  ASSERT_TRUE(read->params[0].type != NULL);
  ASSERT_EQ_INT(read->params[0].type->kind, AT_PATH);
  ASSERT_EQ_INT(read->params[0].type->access, ASTOOLS_ACCESS_READ);
  ASSERT_TRUE(read->params[0].type->must_exist);
  ASSERT_EQ_STR(read->params[1].name, "offset");
  ASSERT_TRUE(!read->params[1].required);
  ASSERT_TRUE(read->params[1].dflt != NULL);
  ASSERT_EQ_INT(read->params[3].type->kind, AT_ENUM);
  ASSERT_EQ_INT(read->params[3].type->values_len, 2);
  ASSERT_TRUE(read->returns_type != NULL);
  ASSERT_EQ_INT(read->returns_type->kind, AT_OBJECT);
  ASSERT_EQ_INT(read->returns_type->fields_len, 3);
  ASSERT_EQ_INT(read->errors_len, 1);
  ASSERT_EQ_STR(read->errors[0].code, "fs/not-found");
  ASSERT_EQ_INT(read->examples_len, 1);
  ASSERT_TRUE(read->examples[0].call != NULL);
  ASSERT_TRUE(read->examples[0].result != NULL);

  ASSERT_TRUE(write->destructive);
  ASSERT_TRUE(!write->read_only);
  ASSERT_EQ_INT(write->timeout_ms, 0); /* unset */

  astools_manifest_free(m);
}

TEST(workspace_null_left_verbatim) {
  astools_manifest *m = parse_ok(FS_MANIFEST, NULL);
  if (!m) return;
  ASSERT_EQ_INT(m->perms.fs_len, 1);
  ASSERT_EQ_STR(m->perms.fs[0].path, "${workspace}");
  astools_manifest_free(m);
}

TEST(reject_bad_id) {
  expect_reject("id: \"fs\"", "id: \"FS\"");
}

TEST(reject_underscore_id) {
  /* D7: no underscore, so <tool>_<command> stays bijective */
  expect_reject("id: \"fs\"", "id: \"my_tool\"");
}

TEST(reject_duplicate_command) {
  expect_reject("name: \"write\"", "name: \"read\"");
}

TEST(reject_manifest_version_2) {
  expect_reject("manifest_version: 1", "manifest_version: 2");
}

TEST(reject_bad_semver) {
  expect_reject("version: \"1.2.0\"", "version: \"1.2\"");
}

TEST(reject_unknown_kind) {
  expect_reject("kind: \"executable\"", "kind: \"script\"");
}

TEST(reject_default_violating_type) {
  /* A default must satisfy its own declared type (§3.4). */
  expect_reject("min: 0 }, required: false, default: 0",
                "min: 0 }, required: false, default: \"zero\"");
}

TEST(reject_garbage) {
  char *err = NULL;
  astools_manifest *m = astools_manifest_parse("not xcdn (", 10, NULL, &err);
  ASSERT_TRUE(m == NULL);
  free(err);
  err = NULL;
  /* well-formed xCDN that is not a #astools_tool object */
  m = astools_manifest_parse("[1, 2]", 6, NULL, &err);
  ASSERT_TRUE(m == NULL);
  free(err);
}

TEST(lint_counts_missing_examples) {
  /* Strip the examples from "write" and expect a warning, zero errors. */
  static const char NO_EX[] =
      "#astools_tool {\n"
      "  manifest_version: 1,\n"
      "  id: \"lintme\",\n"
      "  version: \"1.0.0\",\n"
      "  title: \"Lint me\",\n"
      "  summary: \"A tool without examples.\",\n"
      "  kind: \"executable\",\n"
      "  platforms: [\"linux\"],\n"
      "  runtime: { mode: \"oneshot\", entry: [ { os: \"linux\","
      " arch: \"x86_64\", argv: [\"bin/x\"] } ] },\n"
      "  permissions: { fs: [], net: false, proc: false, env: [] },\n"
      "  commands: [\n"
      "    #command { name: \"go\", summary: \"Go.\", params: [] },\n"
      "  ],\n"
      "}\n";
  astools_buf out;
  astools_manifest *m = parse_ok(NO_EX, NULL);
  int errors;
  if (!m) return;
  astools_buf_init(&out);
  errors = astools_manifest_lint(m, &out);
  ASSERT_EQ_INT(errors, 0);
  ASSERT_TRUE(out.data != NULL);
  ASSERT_TRUE(strstr(out.data, "warn") != NULL);
  ASSERT_TRUE(strstr(out.data, "example") != NULL);
  astools_buf_free(&out);
  astools_manifest_free(m);
}

TEST(render_round_trips) {
  /* Canonical re-serialization must itself parse to the same identity. */
  astools_manifest *m = parse_ok(FS_MANIFEST, NULL);
  char *text = NULL;
  astools_manifest *m2;
  char *err = NULL;
  if (!m) return;
  ASSERT_OK(astools_manifest_render(m, &text));
  ASSERT_TRUE(text != NULL);
  m2 = astools_manifest_parse(text, strlen(text), NULL, &err);
  if (!m2) {
    ASTOOLS_FAILF("re-parse of rendered manifest failed: %s",
                err ? err : "(no message)");
    free(err);
    free(text);
    astools_manifest_free(m);
    return;
  }
  ASSERT_EQ_STR(m2->id, "fs");
  ASSERT_EQ_STR(m2->version, "1.2.0");
  ASSERT_EQ_INT(m2->commands_len, 2);
  astools_manifest_free(m2);
  astools_manifest_free(m);
  free(text);
  free(err);
}

TEST_LIST = {
  TEST_ENTRY(parse_fs_manifest_fields),
  TEST_ENTRY(workspace_null_left_verbatim),
  TEST_ENTRY(reject_bad_id),
  TEST_ENTRY(reject_underscore_id),
  TEST_ENTRY(reject_duplicate_command),
  TEST_ENTRY(reject_manifest_version_2),
  TEST_ENTRY(reject_bad_semver),
  TEST_ENTRY(reject_unknown_kind),
  TEST_ENTRY(reject_default_violating_type),
  TEST_ENTRY(reject_garbage),
  TEST_ENTRY(lint_counts_missing_examples),
  TEST_ENTRY(render_round_trips),
};

RUN_ALL_TESTS()
