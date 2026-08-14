/*
 * test_lockfile.c — lockfile.c: load/check/approve + pinning "enforce"
 * (§4.4, §17).
 */

#include "astools_test.h"

#include "astools_internal.h"
#include "fakes.h"

/* Fixture: registry root with package "lk" (manifest + bin/fake artifact),
 * parsed manifest, and a lockfile path in its own dir. */
typedef struct {
  char root_raw[256], lock_raw[256];
  char pkg_dir[512], lock_path[512], manifest_path[512], artifact[512];
  astools_manifest *m;
} lk_fx;

static int write_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

static int append_file(const char *path, const char *text) {
  FILE *f = fopen(path, "ab");
  if (!f) return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

static int lk_setup(lk_fx *f) {
  char *text;
  size_t len = 0;
  char *err = NULL;
  char bin_dir[512];

  memset(f, 0, sizeof *f);
  if (!astools_test_tmpdir(f->root_raw) || !astools_test_tmpdir(f->lock_raw))
    return 0;
  if (!fake_registry_write(f->root_raw, "lk", NULL, NULL, NULL, NULL, NULL))
    return 0;
  snprintf(f->pkg_dir, sizeof f->pkg_dir, "%s/lk", f->root_raw);
  snprintf(f->manifest_path, sizeof f->manifest_path, "%s/manifest.xcdn",
           f->pkg_dir);
  snprintf(bin_dir, sizeof bin_dir, "%s/bin", f->pkg_dir);
  snprintf(f->artifact, sizeof f->artifact, "%s/bin/fake", f->pkg_dir);
  snprintf(f->lock_path, sizeof f->lock_path, "%s/astools.lock.xcdn",
           f->lock_raw);
  if (os_mkdir_p(bin_dir) != ASTOOLS_OK) return 0;
  if (!write_file(f->artifact, "FAKE-BINARY-CONTENT")) return 0;

  text = astools_test_read_file(f->manifest_path, &len);
  if (!text) return 0;
  f->m = astools_manifest_parse(text, len, NULL, &err);
  free(text);
  free(err);
  return f->m != NULL;
}

static void lk_drop(lk_fx *f) {
  astools_manifest_free(f->m);
  f->m = NULL;
  astools_test_rmtree(f->root_raw);
  astools_test_rmtree(f->lock_raw);
}

TEST(load_missing_file_is_empty) {
  astools_lockfile lf;
  memset(&lf, 0, sizeof lf);
  ASSERT_OK(astools_lockfile_load("/nonexistent/astools.lock.xcdn", &lf));
  ASSERT_EQ_INT(lf.len, 0);
  astools_lockfile_free(&lf);
}

TEST(approve_then_check_ok) {
  lk_fx f;
  astools_lockfile lf;
  memset(&lf, 0, sizeof lf);
  if (!lk_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    lk_drop(&f);
    return;
  }
  if (astools_lockfile_approve(f.lock_path, "lk", "1.0.0", f.pkg_dir, f.m,
                               (astools_time)1700000000) != ASTOOLS_OK) {
    ASTOOLS_FAILF("approve failed");
    lk_drop(&f);
    return;
  }
  if (astools_lockfile_load(f.lock_path, &lf) != ASTOOLS_OK || lf.len != 1) {
    ASTOOLS_FAILF("lockfile load after approve: len=%zu", lf.len);
    astools_lockfile_free(&lf);
    lk_drop(&f);
    return;
  }
  ASSERT_EQ_STR(lf.entries[0].id, "lk");
  ASSERT_EQ_STR(lf.entries[0].version, "1.0.0");
  ASSERT_EQ_INT(lf.entries[0].approved_at, 1700000000);
  ASSERT_EQ_INT(astools_lockfile_check(&lf, "lk", "1.0.0", f.pkg_dir, f.m),
              ASTOOLS_LOCK_OK);
  astools_lockfile_free(&lf);
  lk_drop(&f);
}

TEST(tampered_manifest_is_mismatch) {
  lk_fx f;
  astools_lockfile lf;
  memset(&lf, 0, sizeof lf);
  if (!lk_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    lk_drop(&f);
    return;
  }
  if (astools_lockfile_approve(f.lock_path, "lk", "1.0.0", f.pkg_dir, f.m,
                               (astools_time)1700000000) != ASTOOLS_OK ||
      astools_lockfile_load(f.lock_path, &lf) != ASTOOLS_OK) {
    ASTOOLS_FAILF("approve/load failed");
    astools_lockfile_free(&lf);
    lk_drop(&f);
    return;
  }
  /* one changed byte on disk (an appended comment keeps it parseable) */
  if (!append_file(f.manifest_path, "// tampered\n")) {
    ASTOOLS_FAILF("tamper write failed");
    astools_lockfile_free(&lf);
    lk_drop(&f);
    return;
  }
  ASSERT_EQ_INT(astools_lockfile_check(&lf, "lk", "1.0.0", f.pkg_dir, f.m),
              ASTOOLS_LOCK_MISMATCH);
  astools_lockfile_free(&lf);
  lk_drop(&f);
}

TEST(tampered_artifact_is_mismatch) {
  lk_fx f;
  astools_lockfile lf;
  memset(&lf, 0, sizeof lf);
  if (!lk_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    lk_drop(&f);
    return;
  }
  if (astools_lockfile_approve(f.lock_path, "lk", "1.0.0", f.pkg_dir, f.m,
                               (astools_time)1700000000) != ASTOOLS_OK ||
      astools_lockfile_load(f.lock_path, &lf) != ASTOOLS_OK) {
    ASTOOLS_FAILF("approve/load failed");
    astools_lockfile_free(&lf);
    lk_drop(&f);
    return;
  }
  ASSERT_EQ_INT(astools_lockfile_check(&lf, "lk", "1.0.0", f.pkg_dir, f.m),
              ASTOOLS_LOCK_OK);
  if (!write_file(f.artifact, "EVIL-REPLACEMENT")) {
    ASTOOLS_FAILF("artifact tamper failed");
    astools_lockfile_free(&lf);
    lk_drop(&f);
    return;
  }
  ASSERT_EQ_INT(astools_lockfile_check(&lf, "lk", "1.0.0", f.pkg_dir, f.m),
              ASTOOLS_LOCK_MISMATCH);
  astools_lockfile_free(&lf);
  lk_drop(&f);
}

TEST(missing_entry_is_unlisted) {
  lk_fx f;
  astools_lockfile lf;
  memset(&lf, 0, sizeof lf);
  if (!lk_setup(&f)) {
    ASTOOLS_FAILF("setup failed");
    lk_drop(&f);
    return;
  }
  if (astools_lockfile_approve(f.lock_path, "lk", "1.0.0", f.pkg_dir, f.m,
                               (astools_time)1700000000) != ASTOOLS_OK ||
      astools_lockfile_load(f.lock_path, &lf) != ASTOOLS_OK) {
    ASTOOLS_FAILF("approve/load failed");
    astools_lockfile_free(&lf);
    lk_drop(&f);
    return;
  }
  ASSERT_EQ_INT(
      astools_lockfile_check(&lf, "other", "1.0.0", f.pkg_dir, f.m),
      ASTOOLS_LOCK_UNLISTED);
  ASSERT_EQ_INT(astools_lockfile_check(&lf, "lk", "2.0.0", f.pkg_dir, f.m),
              ASTOOLS_LOCK_UNLISTED);
  astools_lockfile_free(&lf);
  lk_drop(&f);
}

TEST(enforce_blocks_resolve_until_approved) {
  /* §4.4: pinning "enforce" + no lockfile entry => tool disabled; resolve
   * yields DENIED; astools_tool_approve unblocks it. */
  lk_fx f;
  char ws_raw[256], cfg_path[512];
  char cfg[2048];
  astools_ctx *c = NULL;
  astools_tool *t = NULL;
  astools_err e;
  FILE *fp;

  if (!lk_setup(&f) || !astools_test_tmpdir(ws_raw)) {
    ASTOOLS_FAILF("setup failed");
    lk_drop(&f);
    return;
  }
  snprintf(cfg_path, sizeof cfg_path, "%s/config.xcdn", f.lock_raw);
  snprintf(cfg, sizeof cfg,
           "#astools_config {\n"
           "  registry: {\n"
           "    paths: [ { path: \"%s\", trust: \"standard\" } ],\n"
           "    watch: \"off\",\n"
           "    pinning: \"enforce\",\n"
           "  },\n"
           "  workspace: { root: \"%s\" },\n"
           "  logging: { level: \"debug\" },\n"
           "}\n",
           f.root_raw, ws_raw);
  fp = fopen(cfg_path, "wb");
  if (!fp || fputs(cfg, fp) == EOF || fclose(fp) != 0) {
    ASTOOLS_FAILF("config write failed");
    goto out;
  }

  {
    astools_open_params p;
    memset(&p, 0, sizeof p);
    p.config_path = cfg_path;
    if (astools_open(&p, &c) != ASTOOLS_OK) {
      ASTOOLS_FAILF("open failed");
      goto out;
    }
  }

  e = astools_registry_resolve(c, "lk", &t);
  if (e != ASTOOLS_ERR_DENIED) {
    ASTOOLS_FAILF("expected DENIED under enforce, got %s",
                astools_err_name(e));
    if (e == ASTOOLS_OK) astools_tool_unref(t);
    goto out;
  }

  if (astools_tool_approve(c, "lk") != ASTOOLS_OK) {
    ASTOOLS_FAILF("approve via context failed: %s", astools_last_error(c));
    goto out;
  }
  t = NULL;
  e = astools_registry_resolve(c, "lk", &t);
  if (e != ASTOOLS_OK) {
    ASTOOLS_FAILF("resolve after approve failed: %s", astools_err_name(e));
    goto out;
  }
  ASSERT_EQ_INT(t->lock_state, ASTOOLS_LOCK_OK);
  astools_tool_unref(t);

out:
  if (c) astools_close(c);
  astools_test_rmtree(ws_raw);
  lk_drop(&f);
}

TEST_LIST = {
  TEST_ENTRY(load_missing_file_is_empty),
  TEST_ENTRY(approve_then_check_ok),
  TEST_ENTRY(tampered_manifest_is_mismatch),
  TEST_ENTRY(tampered_artifact_is_mismatch),
  TEST_ENTRY(missing_entry_is_unlisted),
  TEST_ENTRY(enforce_blocks_resolve_until_approved),
};

RUN_ALL_TESTS()
