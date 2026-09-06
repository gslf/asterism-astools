/* Real clangd: overloaded definitions and Unicode positions through Astools. */
#include "astools_test.h"
#include "lsp_fixture.h"
static void semantic_case(const char *level) {
  static const char source[] = "int pick(int x) { return x; }\n"
                               "double pick(double x) { return x; }\n"
                               "int use() { /* 😀 */ return pick(1); }\n";
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_CLANGD_PATH, "--background-index=false", source, level));
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "symbols", NULL, 10000, &out);
  int symbols = (int)jx_array_len(jx_object_get(out, "items"));
  if (out) fprintf(stderr, "provider: %s\n", astools_lsp_string(out, "provider"));
  jx_free(out);
  out = NULL;
  if (e != ASTOOLS_OK) {
    lsp_drop(&f);
    ASSERT_OK(e);
  }
  const char *third = strstr(source, "int use"), *call = strstr(third, "pick");
  char position[80];
  snprintf(position, sizeof position, ",line:3,column:%d", (int)(call - third) + 1);
  e = lsp_call(&f, "definition", position, 10000, &out);
  jx_value *items = jx_object_get(out, "items"), *first = jx_array_at(items, 0);
  int definitions = (int)jx_array_len(items),
      line = (int)jx_int_value(jx_object_get(jx_object_get(first, "range"), "start_line"));
  jx_free(out);
  out = NULL;
  if (e != ASTOOLS_OK) {
    lsp_drop(&f);
    ASSERT_OK(e);
  }
  e = lsp_call(&f, "references", position, 10000, &out);
  int references = (int)jx_array_len(jx_object_get(out, "items"));
  jx_free(out);
  lsp_drop(&f);
  ASSERT_OK(e);
  ASSERT_TRUE(symbols >= 3);
  ASSERT_EQ_INT(definitions, 1);
  ASSERT_EQ_INT(line, 1);
  ASSERT_TRUE(references >= 2);
}
TEST(overload_and_unicode) { semantic_case("basic"); }
TEST(strict_semantic_queries) {
  astools_sandbox_caps caps = {0};
  ASSERT_OK(astools_sandbox_caps_impl(1, &caps));
  if (caps.fs_confinement) semantic_case("strict");
  else fprintf(stderr, "strict filesystem enforcement unavailable on this host\n");
}
TEST(diagnostics_are_observations) {
  lsp_fixture f;
  ASSERT_TRUE(lsp_setup(&f, ASTOOLS_CLANGD_PATH, "--background-index=false",
                        "int use() { return missing_symbol; }\n", "basic"));
  jx_value *out = NULL;
  astools_err e = lsp_call(&f, "diagnostics", NULL, 10000, &out);
  int count = (int)jx_array_len(jx_object_get(out, "items"));
  int proof = jx_object_get(out, "verification") != NULL;
  jx_free(out);
  lsp_drop(&f);
  ASSERT_OK(e);
  ASSERT_TRUE(count >= 1);
  ASSERT_TRUE(!proof);
}
TEST_LIST = {TEST_ENTRY(overload_and_unicode), TEST_ENTRY(strict_semantic_queries),
             TEST_ENTRY(diagnostics_are_observations)};
RUN_ALL_TESTS()
