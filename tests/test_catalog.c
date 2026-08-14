/*
 * test_catalog.c — catalog.c: deterministic budgeted rendering (§7.1,
 * §17). Two manifest-only packages (dummy package-relative binaries —
 * catalog never invokes; availability only needs a platform entry).
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "fakes.h"

typedef struct {
  char ws_raw[256], root_raw[256], cfg_raw[256];
  char cfg_path[512];
  astools_ctx *c;
} cat_fx;

/* priority_extra: NULL or a catalog config fragment. */
static int cat_setup(cat_fx *f, const char *extra) {
  astools_open_params p;
  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->ws_raw) || !astools_test_tmpdir(f->root_raw) ||
      !astools_test_tmpdir(f->cfg_raw))
    return 0;
  if (!fake_registry_write(f->root_raw, "aaa", NULL, NULL, NULL, NULL, NULL))
    return 0;
  if (!fake_registry_write(f->root_raw, "zzz", NULL, NULL, NULL, NULL, NULL))
    return 0;
  snprintf(f->cfg_path, sizeof f->cfg_path, "%s/config.xcdn", f->cfg_raw);
  if (!fake_config_write(f->cfg_path, f->root_raw, 0, f->ws_raw, extra))
    return 0;
  memset(&p, 0, sizeof p);
  p.config_path = f->cfg_path;
  return astools_open(&p, &f->c) == ASTOOLS_OK;
}

static void cat_drop(cat_fx *f) {
  if (f->c) astools_close(f->c);
  astools_test_rmtree(f->ws_raw);
  astools_test_rmtree(f->root_raw);
  astools_test_rmtree(f->cfg_raw);
}

TEST(deterministic_byte_identical) {
  cat_fx f;
  char *a = NULL, *b = NULL;
  astools_catalog_level lvl;
  if (!cat_setup(&f, NULL)) {
    ASTOOLS_FAILF("setup failed");
    cat_drop(&f);
    return;
  }
  for (lvl = ASTOOLS_CATALOG_INDEX; lvl <= ASTOOLS_CATALOG_FULL; lvl++) {
    if (astools_catalog(f.c, lvl, 0, &a) != ASTOOLS_OK ||
        astools_catalog(f.c, lvl, 0, &b) != ASTOOLS_OK) {
      ASTOOLS_FAILF("catalog render failed at level %d", (int)lvl);
      astools_free(a);
      astools_free(b);
      cat_drop(&f);
      return;
    }
    if (a == NULL || b == NULL || strcmp(a, b) != 0) {
      ASTOOLS_FAILF("level %d render not byte-identical", (int)lvl);
      astools_free(a);
      astools_free(b);
      cat_drop(&f);
      return;
    }
    astools_free(a);
    astools_free(b);
    a = NULL;
    b = NULL;
  }
  cat_drop(&f);
}

TEST(ordering_by_id_ascending) {
  cat_fx f;
  char *out = NULL;
  const char *pa, *pz;
  if (!cat_setup(&f, NULL)) {
    ASTOOLS_FAILF("setup failed");
    cat_drop(&f);
    return;
  }
  ASSERT_OK(astools_catalog(f.c, ASTOOLS_CATALOG_SUMMARY, 0, &out));
  ASSERT_TRUE(out != NULL);
  pa = strstr(out, "aaa");
  pz = strstr(out, "zzz");
  if (pa == NULL || pz == NULL) {
    ASTOOLS_FAILF("catalog does not mention both tools:\n%s", out);
    astools_free(out);
    cat_drop(&f);
    return;
  }
  if (pa > pz)
    ASTOOLS_FAILF("expected 'aaa' before 'zzz' (id ascending)");
  /* both commands render as <tool>.run */
  ASSERT_TRUE(strstr(out, "aaa.run") != NULL);
  ASSERT_TRUE(strstr(out, "zzz.run") != NULL);
  astools_free(out);
  cat_drop(&f);
}

TEST(priority_override_reorders) {
  /* catalog.priority lists tools first, then id ascending (§7.1) */
  cat_fx f;
  char *out = NULL;
  const char *pa, *pz;
  if (!cat_setup(&f, "catalog: { priority: [\"zzz\"] },")) {
    ASTOOLS_FAILF("setup failed");
    cat_drop(&f);
    return;
  }
  ASSERT_OK(astools_catalog(f.c, ASTOOLS_CATALOG_SUMMARY, 0, &out));
  ASSERT_TRUE(out != NULL);
  pa = strstr(out, "aaa");
  pz = strstr(out, "zzz");
  if (pa == NULL || pz == NULL) {
    ASTOOLS_FAILF("catalog does not mention both tools:\n%s", out);
    astools_free(out);
    cat_drop(&f);
    return;
  }
  if (pz > pa)
    ASTOOLS_FAILF("expected priority tool 'zzz' before 'aaa'");
  astools_free(out);
  cat_drop(&f);
}

TEST(budget_degrades_and_bounds_output) {
  cat_fx f;
  char *full = NULL, *tight = NULL;
  size_t budget = 0;
  if (!cat_setup(&f, NULL)) {
    ASTOOLS_FAILF("setup failed");
    cat_drop(&f);
    return;
  }
  ASSERT_OK(astools_catalog(f.c, ASTOOLS_CATALOG_FULL, 0, &full));
  ASSERT_TRUE(full != NULL && strlen(full) > 0);
  /* a budget below the full render must trim/degrade, never overflow */
  budget = strlen(full) / 2;
  if (budget < 64) budget = 64;
  ASSERT_OK(astools_catalog(f.c, ASTOOLS_CATALOG_FULL, budget, &tight));
  ASSERT_TRUE(tight != NULL);
  if (strlen(tight) > budget)
    ASTOOLS_FAILF("budget %zu exceeded: rendered %zu chars", budget,
                strlen(tight));
  if (strlen(tight) >= strlen(full))
    ASTOOLS_FAILF("budgeted render is not smaller than the full render");
  astools_free(full);
  astools_free(tight);
  cat_drop(&f);
}

TEST(disabled_tool_never_renders) {
  cat_fx f;
  char *out = NULL;
  if (!cat_setup(&f, NULL)) {
    ASTOOLS_FAILF("setup failed");
    cat_drop(&f);
    return;
  }
  ASSERT_OK(astools_tool_enable(f.c, "zzz", 0));
  ASSERT_OK(astools_catalog(f.c, ASTOOLS_CATALOG_SUMMARY, 0, &out));
  ASSERT_TRUE(out != NULL);
  if (strstr(out, "zzz") != NULL) {
    ASTOOLS_FAILF("disabled tool still renders:\n%s", out);
    astools_free(out);
    cat_drop(&f);
    return;
  }
  ASSERT_TRUE(strstr(out, "aaa") != NULL);
  astools_free(out);
  /* re-enable brings it back */
  out = NULL;
  ASSERT_OK(astools_tool_enable(f.c, "zzz", 1));
  ASSERT_OK(astools_catalog(f.c, ASTOOLS_CATALOG_SUMMARY, 0, &out));
  ASSERT_TRUE(out != NULL && strstr(out, "zzz") != NULL);
  astools_free(out);
  cat_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(deterministic_byte_identical),
  TEST_ENTRY(ordering_by_id_ascending),
  TEST_ENTRY(priority_override_reorders),
  TEST_ENTRY(budget_degrades_and_bounds_output),
  TEST_ENTRY(disabled_tool_never_renders),
};

RUN_ALL_TESTS()
