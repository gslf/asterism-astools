/* Binding identity and JSON conversion stay independent of process availability. */
#include "astools_test.h"
#include "mcp_client.h"
#include "mcp_schema.h"
static jx_value *json(const char *text) {
  jx_value *v = NULL;
  (void)jx_parse(text, strlen(text), &v);
  return v;
}
TEST(schema_identity_keeps_precision_keywords_and_full_strings) {
  const char *left[] = {"{\"a\":1,\"b\":2}", "{\"minimum\":9223372036854775807}",
                        "{\"const\":\"x\\u0000y\"}", "{\"required\":[\"a\",\"b\"]}",
                        "{\"const\":1}"};
  const char *right[] = {"{\"b\":2,\"a\":1}", "{\"minimum\":9223372036854775806}",
                         "{\"const\":\"x\\u0000z\"}", "{\"required\":[\"a\"]}", "{\"const\":1.0}"};
  int matches[] = {1, 0, 0, 0, 1};
  for (size_t i = 0; i < 5; i++) {
    jx_value *a = json(left[i]), *b = json(right[i]);
    ASSERT_TRUE(a && b);
    ASSERT_EQ_INT(astools_mcp_schema_equal(a, b), matches[i]);
    jx_free(a);
    jx_free(b);
  }
  jx_value *a = json("{\"const\":9223372036854775807}"),
           *b = json("{\"const\":9.223372036854776e18}");
  ASSERT_TRUE(!astools_mcp_schema_equal(a, b));
  jx_free(a);
  jx_free(b);
}
TEST(non_json_arguments_cannot_be_flattened) {
  const char *inputs[] = {"{msg:\"tè 🍵\",n:9223372036854775807,a:[true,null,2.5]}",
                          "#hidden {msg:\"hello\"}", "{msg:#secret \"hello\"}", "{msg:NaN}"};
  for (size_t i = 0; i < 4; i++) {
    xcdn_error_t error;
    xcdn_document_t *doc = xcdn_parse_str(inputs[i], strlen(inputs[i]), &error);
    jx_value *out = doc ? astools_mcp_arguments(doc->values[0]) : NULL;
    ASSERT_EQ_INT(out != NULL, i == 0);
    if (out) ASSERT_EQ_INT(jx_int_value(jx_object_get(out, "n")), 9223372036854775807LL);
    jx_free(out);
    if (doc) xcdn_document_free(doc);
  }
}
TEST(mcp_bindings_are_explicit_and_never_opt_into_answer_caching) {
  const char *bindings[] = {
      "{\"name\":\"remote.echo\",\"input_schema\":\"{\\\"type\\\":\\\"object\\\"}\"}",
      "{\"name\":\"remote.echo\",\"input_schema\":\"{\\\"type\\\":\\\"object\\\"}\",\"extra\":"
      "true}",
      "{\"name\":\"../escape\",\"input_schema\":\"{\\\"type\\\":\\\"object\\\"}\"}",
      "{\"name\":\"remote.echo\",\"input_schema\":\"{\\\"type\\\":\\\"array\\\"}\"}",
      "{\"name\":\"remote.echo\",\"input_schema\":\"{\\\"type\\\":\\\"object\\\",\\\"type\\\":"
      "\\\"array\\\"}\"}"};
  for (size_t i = 0; i < 5; i++) {
    xcdn_error_t error;
    xcdn_document_t *doc = xcdn_parse_str(bindings[i], strlen(bindings[i]), &error);
    ASSERT_TRUE(doc != NULL);
    astools_manifest manifest = {0};
    manifest.protocol = ASTOOLS_PROTOCOL_MCP;
    astools_cmd cmd = {0};
    ASSERT_EQ_INT(astools_mcp_command_parse(&manifest, &cmd, doc->values[0]), i == 0);
    free(cmd.mcp_name);
    free(cmd.mcp_input_schema);
    free(cmd.mcp_output_schema);
    cmd = (astools_cmd){0};
    cmd.idempotent = true;
    ASSERT_TRUE(!astools_mcp_command_parse(&manifest, &cmd, doc->values[0]));
    xcdn_document_free(doc);
  }
}
TEST(schema_profile_rejects_unsupported_assertions_and_malformed_keywords) {
  const char *invalid[] = {"null",
                           "{\"type\":\"unknown\"}",
                           "{\"type\":[\"null\",\"string\"]}",
                           "{\"$schema\":\"http://json-schema.org/draft-07/schema#\"}",
                           "{\"$ref\":\"http://127.0.0.1/secret\"}",
                           "{\"properties\":{\"s\":{\"pattern\":\".*\"}}}",
                           "{\"required\":[\"x\",\"x\"]}",
                           "{\"enum\":[1,1.0]}",
                           "{\"minItems\":-1}",
                           "{\"maxLength\":1.5}",
                           "{\"items\":[]}",
                           "{\"minimum\":9007199254740993}",
                           "{\"minimum\":\"0\"}",
                           "{\"default\":0,\"unknown\":true}"};
  for (size_t i = 0; i < sizeof invalid / sizeof *invalid; i++) {
    jx_value *schema = json(invalid[i]);
    ASSERT_TRUE(schema != NULL);
    ASSERT_TRUE(!astools_mcp_schema_supported(schema));
    jx_free(schema);
  }
}
TEST(schema_validation_uses_unicode_and_exact_integer_boundaries) {
  const char *schemas[] = {
      "{\"type\":\"string\",\"minLength\":2,\"maxLength\":2}",
      "{\"type\":\"integer\",\"exclusiveMinimum\":-2,\"maximum\":9007199254740992}",
      "{\"type\":\"object\",\"properties\":{\"a\":{\"type\":\"array\",\"items\":{\"enum\":[1,2]},"
      "\"maxItems\":2}},\"required\":[\"a\"],\"additionalProperties\":false}",
      "{\"type\":\"array\",\"minItems\":1,\"items\":false}",
      "{\"const\":null}",
      "{\"type\":\"integer\"}"};
  const char *valid[] = {"\"è🍵\"", "9007199254740992", "{\"a\":[1,2]}", NULL, "null", "2.0"};
  const char *invalid[] = {
      "\"é🍵\"", "9007199254740993", "{\"a\":[1],\"extra\":null}", "[null]", "false", "2.5"};
  for (size_t i = 0; i < 6; i++) {
    jx_value *schema = json(schemas[i]), *bad = json(invalid[i]);
    ASSERT_TRUE(astools_mcp_schema_supported(schema));
    ASSERT_TRUE(!astools_mcp_schema_validate(schema, bad));
    if (valid[i]) {
      jx_value *good = json(valid[i]);
      ASSERT_TRUE(astools_mcp_schema_validate(schema, good));
      jx_free(good);
    }
    jx_free(schema);
    jx_free(bad);
  }
}
TEST(schema_and_instance_limits_are_enforced) {
  jx_value *schema = jx_bool(1);
  for (int i = 0; i < 34; i++) {
    jx_value *outer = jx_object();
    jx_object_set(outer, "items", schema);
    schema = outer;
  }
  ASSERT_TRUE(!astools_mcp_schema_supported(schema));
  jx_free(schema);
  schema = json("{\"type\":\"array\",\"items\":{\"type\":\"integer\"}}");
  jx_value *large = jx_array();
  for (int i = 0; i < 32768; i++) ASSERT_EQ_INT(jx_array_push(large, jx_int(i)), 0);
  ASSERT_TRUE(!astools_mcp_schema_validate(schema, large));
  jx_free(schema);
  jx_free(large);
}
TEST_LIST = {TEST_ENTRY(schema_profile_rejects_unsupported_assertions_and_malformed_keywords),
             TEST_ENTRY(schema_validation_uses_unicode_and_exact_integer_boundaries),
             TEST_ENTRY(schema_and_instance_limits_are_enforced),
             TEST_ENTRY(schema_identity_keeps_precision_keywords_and_full_strings),
             TEST_ENTRY(non_json_arguments_cannot_be_flattened),
             TEST_ENTRY(mcp_bindings_are_explicit_and_never_opt_into_answer_caching)};
RUN_ALL_TESTS()
