/*
 * test_types.c — types.c: #type parsing, strict validation, defaults
 *. Every kind gets accept + reject cases; validation is
 * strict with NO coercion.
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "xcdn.h"

/* Sentinel for "the fixture itself failed to parse" — distinct from any
 * astools_err the checker can return. */
#define FIX_BROKEN ((astools_err)-1)

/* Parse a #type source, then check a value source against it. Frees
 * everything before returning the checker verdict. */
static astools_err check_tv(const char *type_src, const char *val_src) {
  xcdn_error_t xe;
  char err[512];
  xcdn_document_t *td = NULL, *vd = NULL;
  astools_type *t = NULL;
  astools_err e = FIX_BROKEN;

  memset(&xe, 0, sizeof xe);
  td = xcdn_parse(type_src, &xe);
  if (!td || td->values_len == 0) goto done;
  t = astools_type_parse(td->values[0], err, sizeof err);
  if (!t) goto done;
  memset(&xe, 0, sizeof xe);
  vd = xcdn_parse(val_src, &xe);
  if (!vd || vd->values_len == 0) {
    e = ASTOOLS_ERR_PARSE; /* literal unrepresentable in xCDN wire text */
    goto done;
  }
  err[0] = '\0';
  e = astools_type_check(t, vd->values[0], "x", err, sizeof err);

done:
  astools_type_free(t);
  if (td) xcdn_document_free(td);
  if (vd) xcdn_document_free(vd);
  return e;
}

#define ACCEPT(ty, val) ASSERT_ERR(check_tv((ty), (val)), ASTOOLS_OK)
#define REJECT(ty, val) ASSERT_ERR(check_tv((ty), (val)), ASTOOLS_ERR_INVALID)

/* 1 when the #type source itself is rejected by astools_type_parse. */
static int type_rejected(const char *type_src) {
  xcdn_error_t xe;
  char err[512];
  xcdn_document_t *td;
  astools_type *t;
  memset(&xe, 0, sizeof xe);
  td = xcdn_parse(type_src, &xe);
  if (!td || td->values_len == 0) {
    if (td) xcdn_document_free(td);
    return -1;
  }
  t = astools_type_parse(td->values[0], err, sizeof err);
  astools_type_free(t);
  xcdn_document_free(td);
  return t == NULL;
}

/* ---- scalar kinds -------------------------------------------------------- */

TEST(kind_string) {
  ACCEPT("{ kind: \"string\" }", "\"hi\"");
  ACCEPT("{ kind: \"string\" }", "\"\"");
  REJECT("{ kind: \"string\" }", "3");
  REJECT("{ kind: \"string\" }", "true");
  REJECT("{ kind: \"string\" }", "{}");
  REJECT("{ kind: \"string\" }", "[]");
  REJECT("{ kind: \"string\" }", "null");
  /* length constraints */
  ACCEPT("{ kind: \"string\", min_len: 2, max_len: 3 }", "\"ab\"");
  ACCEPT("{ kind: \"string\", min_len: 2, max_len: 3 }", "\"abc\"");
  REJECT("{ kind: \"string\", min_len: 2, max_len: 3 }", "\"a\"");
  REJECT("{ kind: \"string\", min_len: 2, max_len: 3 }", "\"abcd\"");
}

TEST(kind_integer) {
  ACCEPT("{ kind: \"integer\" }", "5");
  ACCEPT("{ kind: \"integer\" }", "-5");
  /* strict, no coercion: "3" is not an integer, nor is a float literal */
  REJECT("{ kind: \"integer\" }", "\"3\"");
  REJECT("{ kind: \"integer\" }", "3.0");
  REJECT("{ kind: \"integer\" }", "true");
  REJECT("{ kind: \"integer\" }", "null");
  ACCEPT("{ kind: \"integer\", min: 0, max: 10 }", "0");
  ACCEPT("{ kind: \"integer\", min: 0, max: 10 }", "10");
  REJECT("{ kind: \"integer\", min: 0, max: 10 }", "-1");
  REJECT("{ kind: \"integer\", min: 0, max: 10 }", "11");
}

TEST(kind_number) {
  ACCEPT("{ kind: \"number\" }", "1.5");
  ACCEPT("{ kind: \"number\" }", "-0.25");
  /* number accepts an integer literal (an int IS a number)… */
  ACCEPT("{ kind: \"number\" }", "1");
  /* …but never a string or bool */
  REJECT("{ kind: \"number\" }", "\"1.5\"");
  REJECT("{ kind: \"number\" }", "true");
  ACCEPT("{ kind: \"number\", min: 0.5, max: 2.5 }", "0.5");
  ACCEPT("{ kind: \"number\", min: 0.5, max: 2.5 }", "2.5");
  ACCEPT("{ kind: \"number\", min: 0.5, max: 2.5 }", "1");
  REJECT("{ kind: \"number\", min: 0.5, max: 2.5 }", "0.25");
  REJECT("{ kind: \"number\", min: 0.5, max: 2.5 }", "3.5");
}

TEST(kind_boolean) {
  ACCEPT("{ kind: \"boolean\" }", "true");
  ACCEPT("{ kind: \"boolean\" }", "false");
  /* 1 is not true */
  REJECT("{ kind: \"boolean\" }", "1");
  REJECT("{ kind: \"boolean\" }", "0");
  REJECT("{ kind: \"boolean\" }", "\"true\"");
  REJECT("{ kind: \"boolean\" }", "null");
}

TEST(kind_bytes) {
  ACCEPT("{ kind: \"bytes\" }", "b\"aGk=\"");
  ACCEPT("{ kind: \"bytes\" }", "b\"\"");
  REJECT("{ kind: \"bytes\" }", "\"aGk=\""); /* plain string is not bytes */
  REJECT("{ kind: \"bytes\" }", "3");
  /* max_len counts DECODED bytes: "aGk=" = 2, "aGlp" = 3 */
  ACCEPT("{ kind: \"bytes\", max_len: 2 }", "b\"aGk=\"");
  REJECT("{ kind: \"bytes\", max_len: 2 }", "b\"aGlp\"");
}

TEST(kind_datetime) {
  ACCEPT("{ kind: \"datetime\" }", "t\"2026-01-01T00:00:00Z\"");
  ACCEPT("{ kind: \"datetime\" }", "t\"2026-01-01T02:00:00+02:00\"");
  /* plain string never satisfies datetime (strict) */
  REJECT("{ kind: \"datetime\" }", "\"2026-01-01T00:00:00Z\"");
  /* contract: — t"…" content must be valid RFC 3339 */
  REJECT("{ kind: \"datetime\" }", "t\"not-a-date\"");
  REJECT("{ kind: \"datetime\" }", "t\"2026-13-01T00:00:00Z\"");
}

TEST(kind_duration) {
  ACCEPT("{ kind: \"duration\" }", "r\"PT30S\"");
  ACCEPT("{ kind: \"duration\" }", "r\"P1DT1S\"");
  REJECT("{ kind: \"duration\" }", "\"PT30S\"");
  /* contract: — r"…" content must be a valid ISO 8601 duration
   * (months/years rejected per the time.c contract) */
  REJECT("{ kind: \"duration\" }", "r\"nope\"");
  REJECT("{ kind: \"duration\" }", "r\"P1M\"");
}

TEST(kind_uuid) {
  ACCEPT("{ kind: \"uuid\" }",
         "u\"550e8400-e29b-41d4-a716-446655440000\"");
  REJECT("{ kind: \"uuid\" }", "\"550e8400-e29b-41d4-a716-446655440000\"");
  /* The xcdn lexer already refuses malformed u"…" literals, so the
   * invalid UUID is unrepresentable in wire text (types.c still guards
   * programmatically built nodes). */
  ASSERT_ERR(check_tv("{ kind: \"uuid\" }", "u\"xyz\""), ASTOOLS_ERR_PARSE);
  REJECT("{ kind: \"uuid\" }", "3");
}

TEST(kind_path) {
  /* Path params check as strings here; resolution happens in policy
   * (types.c contract). */
  ACCEPT("{ kind: \"path\", access: \"read\" }", "\"a/b.txt\"");
  ACCEPT("{ kind: \"path\", access: \"read-write\", must_exist: true }",
         "\"x\"");
  REJECT("{ kind: \"path\", access: \"read\" }", "3");
  REJECT("{ kind: \"path\", access: \"read\" }", "true");
  REJECT("{ kind: \"path\", access: \"read\" }", "null");
}

TEST(kind_enum) {
  ACCEPT("{ kind: \"enum\", values: [\"a\", \"b\"] }", "\"a\"");
  ACCEPT("{ kind: \"enum\", values: [\"a\", \"b\"] }", "\"b\"");
  REJECT("{ kind: \"enum\", values: [\"a\", \"b\"] }", "\"c\"");
  REJECT("{ kind: \"enum\", values: [\"a\", \"b\"] }", "\"A\"");
  REJECT("{ kind: \"enum\", values: [\"a\", \"b\"] }", "1");
}

/* ---- container kinds ----------------------------------------------------- */

TEST(kind_array) {
  static const char TY[] =
      "{ kind: \"array\", item: { kind: \"integer\" },"
      " min_items: 1, max_items: 2 }";
  ACCEPT(TY, "[1]");
  ACCEPT(TY, "[1, 2]");
  REJECT(TY, "[]");        /* min_items */
  REJECT(TY, "[1, 2, 3]"); /* max_items */
  REJECT(TY, "[\"x\"]");   /* item type */
  REJECT(TY, "\"x\"");
  REJECT(TY, "{}");
  /* nested item constraint enforced recursively */
  ACCEPT("{ kind: \"array\", item: { kind: \"integer\", min: 0 } }",
         "[0, 1]");
  REJECT("{ kind: \"array\", item: { kind: \"integer\", min: 0 } }",
         "[0, -1]");
}

TEST(kind_map) {
  static const char TY[] =
      "{ kind: \"map\", value: { kind: \"string\" }, max_entries: 2 }";
  ACCEPT(TY, "{}");
  ACCEPT(TY, "{ a: \"x\", b: \"y\" }");
  ACCEPT(TY, "{ \"weird key!\": \"x\" }"); /* arbitrary string keys */
  REJECT(TY, "{ a: 1 }");                  /* value type */
  REJECT(TY, "{ a: \"x\", b: \"y\", c: \"z\" }"); /* max_entries */
  REJECT(TY, "[]");
}

TEST(kind_object) {
  static const char TY[] =
      "{ kind: \"object\", fields: ["
      " #param { name: \"x\", type: { kind: \"integer\" }, required: true },"
      " #param { name: \"y\", type: { kind: \"string\" }, required: false },"
      " ] }";
  ACCEPT(TY, "{ x: 1 }");
  ACCEPT(TY, "{ x: 1, y: \"s\" }");
  REJECT(TY, "{}");                /* missing required */
  REJECT(TY, "{ y: \"s\" }");      /* missing required */
  REJECT(TY, "{ x: 1, z: 2 }");    /* closed object: unknown key */
  REJECT(TY, "{ x: \"s\" }");      /* field type */
  REJECT(TY, "[]");
}

/* ---- type parse rejects --------------------------------------------------- */

TEST(type_parse_rejects) {
  ASSERT_EQ_INT(type_rejected("{ kind: \"wat\" }"), 1);
  ASSERT_EQ_INT(type_rejected("{}"), 1);
  ASSERT_EQ_INT(type_rejected("{ kind: 3 }"), 1);
  ASSERT_EQ_INT(type_rejected("\"string\""), 1); /* not a #type object */
  /* array without an item type is unusable */
  ASSERT_EQ_INT(type_rejected("{ kind: \"array\" }"), 1);
  /* sanity: a good one parses */
  ASSERT_EQ_INT(type_rejected("{ kind: \"string\" }"), 0);
}

/* ---- args validation: required / unknown / defaults ----------------------- */

/* Builds a two-param command over parsed types:
 *   a: integer, required
 *   b: string,  optional, default "dee"
 *   o: object { x: integer required, y: integer optional default 9 }, optional
 */
typedef struct {
  xcdn_document_t *docs[8];
  size_t docs_n;
  astools_type *types[8];
  size_t types_n;
  astools_param params[3];
  astools_cmd cmd;
} cmd_fixture;

static astools_type *fx_type(cmd_fixture *fx, const char *src) {
  xcdn_error_t xe;
  char err[512];
  xcdn_document_t *d;
  astools_type *t;
  memset(&xe, 0, sizeof xe);
  d = xcdn_parse(src, &xe);
  if (!d) return NULL;
  fx->docs[fx->docs_n++] = d;
  t = astools_type_parse(d->values[0], err, sizeof err);
  if (t) fx->types[fx->types_n++] = t;
  return t;
}

static xcdn_node_t *fx_node(cmd_fixture *fx, const char *src) {
  xcdn_error_t xe;
  xcdn_document_t *d;
  memset(&xe, 0, sizeof xe);
  d = xcdn_parse(src, &xe);
  if (!d || d->values_len == 0) {
    if (d) xcdn_document_free(d);
    return NULL;
  }
  fx->docs[fx->docs_n++] = d;
  return d->values[0];
}

static int fx_build(cmd_fixture *fx) {
  astools_type *ta, *tb, *to;
  xcdn_node_t *db;
  memset(fx, 0, sizeof *fx);
  ta = fx_type(fx, "{ kind: \"integer\" }");
  tb = fx_type(fx, "{ kind: \"string\" }");
  to = fx_type(fx,
               "{ kind: \"object\", fields: ["
               " #param { name: \"x\", type: { kind: \"integer\" },"
               " required: true },"
               " #param { name: \"y\", type: { kind: \"integer\" },"
               " required: false, default: 9 },"
               " ] }");
  db = fx_node(fx, "\"dee\"");
  if (!ta || !tb || !to || !db) return 0;
  fx->params[0].name = (char *)"a";
  fx->params[0].type = ta;
  fx->params[0].required = true;
  fx->params[1].name = (char *)"b";
  fx->params[1].type = tb;
  fx->params[1].required = false;
  fx->params[1].dflt = db;
  fx->params[2].name = (char *)"o";
  fx->params[2].type = to;
  fx->params[2].required = false;
  fx->cmd.name = (char *)"run";
  fx->cmd.params = fx->params;
  fx->cmd.params_len = 3;
  return 1;
}

static void fx_free(cmd_fixture *fx) {
  size_t i;
  for (i = 0; i < fx->types_n; i++) astools_type_free(fx->types[i]);
  for (i = 0; i < fx->docs_n; i++) xcdn_document_free(fx->docs[i]);
}

static const char *node_str(const xcdn_node_t *n) {
  if (!n || !n->value || n->value->type != XCDN_VAL_STRING) return NULL;
  return n->value->data.string;
}

TEST(args_validate_happy_and_defaults) {
  cmd_fixture fx;
  xcdn_node_t *args;
  const xcdn_node_t *b;
  char err[512];
  if (!fx_build(&fx)) {
    ASTOOLS_FAILF("fixture build failed");
    fx_free(&fx);
    return;
  }
  args = fx_node(&fx, "{ a: 1 }");
  if (!args) {
    ASTOOLS_FAILF("args parse failed");
    fx_free(&fx);
    return;
  }
  err[0] = '\0';
  if (astools_args_validate(&fx.cmd, args, err, sizeof err) != ASTOOLS_OK) {
    ASTOOLS_FAILF("validate failed: %s", err);
    fx_free(&fx);
    return;
  }
  /* default injected for the absent optional */
  b = xcdn_object_get(args->value, "b");
  if (!b || node_str(b) == NULL || strcmp(node_str(b), "dee") != 0) {
    ASTOOLS_FAILF("default for 'b' not injected");
    fx_free(&fx);
    return;
  }
  fx_free(&fx);
}

TEST(args_validate_nested_default) {
  /* contract: tools always see a COMPLETE canonical args
   * object, so defaults inject recursively into present object params. */
  cmd_fixture fx;
  xcdn_node_t *args;
  const xcdn_node_t *o, *y;
  char err[512];
  if (!fx_build(&fx)) {
    ASTOOLS_FAILF("fixture build failed");
    fx_free(&fx);
    return;
  }
  args = fx_node(&fx, "{ a: 1, o: { x: 2 } }");
  if (!args) {
    ASTOOLS_FAILF("args parse failed");
    fx_free(&fx);
    return;
  }
  err[0] = '\0';
  if (astools_args_validate(&fx.cmd, args, err, sizeof err) != ASTOOLS_OK) {
    ASTOOLS_FAILF("validate failed: %s", err);
    fx_free(&fx);
    return;
  }
  o = xcdn_object_get(args->value, "o");
  y = o && o->value ? xcdn_object_get(o->value, "y") : NULL;
  if (!y || !y->value || y->value->type != XCDN_VAL_INT ||
      y->value->data.integer != 9) {
    ASTOOLS_FAILF("nested default o.y = 9 not injected");
    fx_free(&fx);
    return;
  }
  fx_free(&fx);
}

TEST(args_validate_rejects) {
  cmd_fixture fx;
  xcdn_node_t *args;
  char err[512];
  if (!fx_build(&fx)) {
    ASTOOLS_FAILF("fixture build failed");
    fx_free(&fx);
    return;
  }

  /* missing required */
  args = fx_node(&fx, "{ b: \"x\" }");
  err[0] = '\0';
  ASSERT_TRUE(args != NULL);
  ASSERT_ERR(astools_args_validate(&fx.cmd, args, err, sizeof err),
             ASTOOLS_ERR_INVALID);
  ASSERT_TRUE(strstr(err, "a") != NULL); /* names the parameter */

  /* unknown key rejected (closed args) */
  args = fx_node(&fx, "{ a: 1, nope: 2 }");
  err[0] = '\0';
  ASSERT_TRUE(args != NULL);
  ASSERT_ERR(astools_args_validate(&fx.cmd, args, err, sizeof err),
             ASTOOLS_ERR_INVALID);

  /* wrong type, no coercion */
  args = fx_node(&fx, "{ a: \"1\" }");
  err[0] = '\0';
  ASSERT_TRUE(args != NULL);
  ASSERT_ERR(astools_args_validate(&fx.cmd, args, err, sizeof err),
             ASTOOLS_ERR_INVALID);

  fx_free(&fx);
}

/* Regression: hostile tool output (deep nesting) must not overflow the C
 * stack in the xCDN parser — it must be rejected as a parse error. A
 * compromised tool's stdout is attacker-controlled. */
TEST(parser_depth_cap_rejects_deep_nesting) {
  size_t depth = 100000, i;
  char *buf = malloc(depth * 2 + 2);
  xcdn_error_t xe;
  xcdn_document_t *d;
  ASSERT_TRUE(buf != NULL);
  for (i = 0; i < depth; i++) buf[i] = '[';
  buf[depth] = '1';
  for (i = 0; i < depth; i++) buf[depth + 1 + i] = ']';
  buf[depth * 2 + 1] = '\0';
  memset(&xe, 0, sizeof xe);
  d = xcdn_parse(buf, &xe); /* must return NULL, not crash */
  ASSERT_TRUE(d == NULL);
  if (d) xcdn_document_free(d);
  free(buf);
}

/* A legitimately nested document (well under the cap) still parses. */
TEST(parser_accepts_reasonable_nesting) {
  size_t depth = 64, i;
  char *buf = malloc(depth * 2 + 2);
  xcdn_error_t xe;
  xcdn_document_t *d;
  ASSERT_TRUE(buf != NULL);
  for (i = 0; i < depth; i++) buf[i] = '[';
  buf[depth] = '1';
  for (i = 0; i < depth; i++) buf[depth + 1 + i] = ']';
  buf[depth * 2 + 1] = '\0';
  memset(&xe, 0, sizeof xe);
  d = xcdn_parse(buf, &xe);
  ASSERT_TRUE(d != NULL);
  if (d) xcdn_document_free(d);
  free(buf);
}

TEST_LIST = {
  TEST_ENTRY(kind_string),
  TEST_ENTRY(kind_integer),
  TEST_ENTRY(kind_number),
  TEST_ENTRY(kind_boolean),
  TEST_ENTRY(kind_bytes),
  TEST_ENTRY(kind_datetime),
  TEST_ENTRY(kind_duration),
  TEST_ENTRY(kind_uuid),
  TEST_ENTRY(kind_path),
  TEST_ENTRY(kind_enum),
  TEST_ENTRY(kind_array),
  TEST_ENTRY(kind_map),
  TEST_ENTRY(kind_object),
  TEST_ENTRY(type_parse_rejects),
  TEST_ENTRY(args_validate_happy_and_defaults),
  TEST_ENTRY(args_validate_nested_default),
  TEST_ENTRY(args_validate_rejects),
  TEST_ENTRY(parser_depth_cap_rejects_deep_nesting),
  TEST_ENTRY(parser_accepts_reasonable_nesting),
};

RUN_ALL_TESTS()
