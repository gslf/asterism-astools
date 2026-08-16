/*
 * test_jscm.c — jscm.c: JSON Schema generation + MCP description. The
 * exact-string compare over a command with EVERY kind
 * represented doubles as the golden: any drift in the deterministic
 * rendering breaks this test on purpose.
 */

#include "astools_test.h"

#include "astools_internal.h"

static const char ALL_KINDS_MANIFEST[] =
    "#astools_tool {\n"
    "  manifest_version: 1,\n"
    "  id: \"kinds\",\n"
    "  version: \"1.0.0\",\n"
    "  title: \"Every kind\",\n"
    "  summary: \"One command with every #type kind.\",\n"
    "  kind: \"executable\",\n"
    "  platforms: [\"linux\"],\n"
    "  runtime: { mode: \"oneshot\", entry: [ { os: \"linux\","
    " arch: \"x86_64\", argv: [\"bin/kinds\"] } ] },\n"
    "  permissions: { fs: [ { path: \"${workspace}\","
    " access: \"read-write\" } ], net: false, proc: false, env: [] },\n"
    "  commands: [\n"
    "    #command {\n"
    "      name: \"all\",\n"
    "      summary: \"Exercise every kind.\",\n"
    "      annotations: { read_only: true },\n"
    "      params: [\n"
    "        #param { name: \"s\", type: #type { kind: \"string\","
    " min_len: 1, max_len: 8 }, required: true,"
    " description: \"a string\" },\n"
    "        #param { name: \"i\", type: #type { kind: \"integer\","
    " min: 0, max: 100 }, required: false, default: 5 },\n"
    "        #param { name: \"n\", type: #type { kind: \"number\","
    " min: 0.5 }, required: false },\n"
    "        #param { name: \"f\", type: #type { kind: \"boolean\" },"
    " required: false },\n"
    "        #param { name: \"by\", type: #type { kind: \"bytes\" },"
    " required: false },\n"
    "        #param { name: \"dt\", type: #type { kind: \"datetime\" },"
    " required: false },\n"
    "        #param { name: \"du\", type: #type { kind: \"duration\" },"
    " required: false },\n"
    "        #param { name: \"id\", type: #type { kind: \"uuid\" },"
    " required: false },\n"
    "        #param { name: \"p\", type: #type { kind: \"path\","
    " access: \"read-write\", must_exist: true }, required: false },\n"
    "        #param { name: \"e\", type: #type { kind: \"enum\","
    " values: [\"a\", \"b\"] }, required: false },\n"
    "        #param { name: \"arr\", type: #type { kind: \"array\","
    " item: #type { kind: \"integer\" }, min_items: 1, max_items: 3 },"
    " required: false },\n"
    "        #param { name: \"m\", type: #type { kind: \"map\","
    " value: #type { kind: \"string\" }, max_entries: 4 },"
    " required: false },\n"
    "        #param { name: \"o\", type: #type { kind: \"object\","
    " fields: [\n"
    "          #param { name: \"x\", type: #type { kind: \"integer\" },"
    " required: true },\n"
    "          #param { name: \"y\", type: #type { kind: \"string\" },"
    " required: false },\n"
    "        ] }, required: false },\n"
    "      ],\n"
    "      examples: [ #example { call: { s: \"hi\" },"
    " result: { ok: true } } ],\n"
    "    },\n"
    "  ],\n"
    "}\n";

/* Golden: fixed key order (type/enum, description, default, constraints,
 * then items/properties), "required" always present, additionalProperties
 * false on closed objects. */
static const char GOLDEN_SCHEMA[] =
    "{\"type\":\"object\","
    "\"properties\":{"
    "\"s\":{\"type\":\"string\",\"description\":\"a string\","
    "\"maxLength\":8,\"minLength\":1},"
    "\"i\":{\"type\":\"integer\",\"default\":5,"
    "\"maximum\":100,\"minimum\":0},"
    "\"n\":{\"type\":\"number\",\"minimum\":0.5},"
    "\"f\":{\"type\":\"boolean\"},"
    "\"by\":{\"type\":\"string\",\"contentEncoding\":\"base64\"},"
    "\"dt\":{\"type\":\"string\",\"format\":\"date-time\"},"
    "\"du\":{\"type\":\"string\",\"format\":\"duration\"},"
    "\"id\":{\"type\":\"string\",\"format\":\"uuid\"},"
    "\"p\":{\"type\":\"string\",\"description\":\"workspace-relative path;"
    " access: read-write, must exist\"},"
    "\"e\":{\"enum\":[\"a\",\"b\"]},"
    "\"arr\":{\"type\":\"array\",\"maxItems\":3,\"minItems\":1,"
    "\"items\":{\"type\":\"integer\"}},"
    "\"m\":{\"type\":\"object\",\"maxProperties\":4,"
    "\"additionalProperties\":{\"type\":\"string\"}},"
    "\"o\":{\"type\":\"object\",\"properties\":{"
    "\"x\":{\"type\":\"integer\"},"
    "\"y\":{\"type\":\"string\"}},"
    "\"required\":[\"x\"],\"additionalProperties\":false}"
    "},"
    "\"required\":[\"s\"],"
    "\"additionalProperties\":false}";

static astools_manifest *parse_fixture(void) {
  char *err = NULL;
  astools_manifest *m = astools_manifest_parse(
      ALL_KINDS_MANIFEST, strlen(ALL_KINDS_MANIFEST), NULL, &err);
  if (!m) {
    ASTOOLS_FAILF("fixture manifest rejected: %s",
                err ? err : "(no message)");
    free(err);
    return NULL;
  }
  free(err);
  return m;
}

TEST(golden_input_schema_every_kind) {
  astools_manifest *m = parse_fixture();
  const astools_cmd *cmd;
  char *json = NULL;
  if (!m) return;
  cmd = astools_manifest_cmd(m, "all");
  if (!cmd) {
    ASTOOLS_FAILF("command 'all' missing");
    astools_manifest_free(m);
    return;
  }
  if (astools_jschema_input(cmd, &json) != ASTOOLS_OK || json == NULL) {
    ASTOOLS_FAILF("jschema_input failed");
    astools_manifest_free(m);
    return;
  }
  ASSERT_EQ_STR(json, GOLDEN_SCHEMA);
  free(json);
  astools_manifest_free(m);
}

TEST(schema_is_deterministic) {
  astools_manifest *m = parse_fixture();
  const astools_cmd *cmd;
  char *a = NULL, *b = NULL;
  if (!m) return;
  cmd = astools_manifest_cmd(m, "all");
  ASSERT_TRUE(cmd != NULL);
  ASSERT_OK(astools_jschema_input(cmd, &a));
  ASSERT_OK(astools_jschema_input(cmd, &b));
  ASSERT_EQ_STR(a, b); /* identical input => byte-identical output */
  free(a);
  free(b);
  astools_manifest_free(m);
}

TEST(schema_empty_params) {
  /* No params: empty properties, empty (but present) required. */
  static const char EMPTY[] =
      "#astools_tool {\n"
      "  manifest_version: 1,\n"
      "  id: \"noargs\",\n"
      "  version: \"1.0.0\",\n"
      "  title: \"No args\",\n"
      "  summary: \"A command without parameters.\",\n"
      "  kind: \"executable\",\n"
      "  platforms: [\"linux\"],\n"
      "  runtime: { mode: \"oneshot\", entry: [ { os: \"linux\","
      " arch: \"x86_64\", argv: [\"bin/x\"] } ] },\n"
      "  permissions: { fs: [], net: false, proc: false, env: [] },\n"
      "  commands: [ #command { name: \"go\", summary: \"Go.\","
      " params: [], examples: [ #example { call: {}, result: {} } ] } ],\n"
      "}\n";
  char *err = NULL, *json = NULL;
  astools_manifest *m =
      astools_manifest_parse(EMPTY, strlen(EMPTY), NULL, &err);
  if (!m) {
    ASTOOLS_FAILF("fixture rejected: %s", err ? err : "(no message)");
    free(err);
    return;
  }
  free(err);
  ASSERT_OK(astools_jschema_input(astools_manifest_cmd(m, "go"), &json));
  ASSERT_EQ_STR(json,
              "{\"type\":\"object\",\"properties\":{},\"required\":[],"
              "\"additionalProperties\":false}");
  free(json);
  astools_manifest_free(m);
}

TEST(mcp_description_composed) {
  /* Composed description: tool summary + command bits + rendered example
   * + permission note. Substring checks — the golden above pins
   * the byte-exact schema; the description just has to carry the parts. */
  astools_manifest *m = parse_fixture();
  astools_tool t;
  const astools_cmd *cmd;
  char *text = NULL;
  if (!m) return;
  memset(&t, 0, sizeof t);
  t.m = m;
  cmd = astools_manifest_cmd(m, "all");
  ASSERT_TRUE(cmd != NULL);
  if (astools_mcp_description(&t, cmd, &text) != ASTOOLS_OK ||
      text == NULL) {
    ASTOOLS_FAILF("mcp_description failed");
    astools_manifest_free(m);
    return;
  }
  ASSERT_TRUE(strstr(text, "One command with every #type kind.") != NULL);
  ASSERT_TRUE(strstr(text, "Exercise every kind.") != NULL);
  ASSERT_TRUE(strstr(text, "Example: kinds.all") != NULL);
  free(text);
  astools_manifest_free(m);
}

TEST_LIST = {
  TEST_ENTRY(golden_input_schema_every_kind),
  TEST_ENTRY(schema_is_deterministic),
  TEST_ENTRY(schema_empty_params),
  TEST_ENTRY(mcp_description_composed),
};

RUN_ALL_TESTS()
