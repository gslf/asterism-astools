/*
 * registry.c — roots scan, table publish, resolution (§4).
 *
 * Scan protocol (§4.2): candidates are built and hashed without holding
 * c->lock (cfg is immutable after open); the new table, lockfile snapshot,
 * fingerprint and stats are published in one write-locked swap, and the
 * replaced descriptors are unrefed only after the lock is released.
 *
 * Manifests come from arbitrary directories and are hostile input: every
 * rejection is a warning, never a failure of the scan itself (G7).
 */

#include "astools_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---- current platform (compile-time) ------------------------------------- */

const char *astools_plat_os(void) {
#if defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#else
  return "linux";
#endif
}

const char *astools_plat_arch(void) {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  return "arm64";
#else
  return "x86_64";
#endif
}

/* ---- tool refcounting ---------------------------------------------------- */

/* One process-wide mutex serializes refcount updates across contexts:
 * resolve refs under a SHARED read lock (no mutual exclusion) and
 * invocations unref with no context lock held at all (§9). */
#if defined(ASTOOLS_NO_THREADS)

static void ref_global_init(void) {}
static void ref_lock(void) {}
static void ref_unlock(void) {}

#elif !defined(_WIN32)

/* POSIX: race-free static initialization. */
static pthread_mutex_t g_ref_mu = PTHREAD_MUTEX_INITIALIZER;
static void ref_global_init(void) {}
static void ref_lock(void) { (void)pthread_mutex_lock(&g_ref_mu); }
static void ref_unlock(void) { (void)pthread_mutex_unlock(&g_ref_mu); }

#else

/* No portable static initializer for os_mutex: boot it from the first
 * registry scan. Every context scans at open before any resolve/unref can
 * run, so the only race is two first-ever scans in different contexts
 * opening concurrently — an accepted residual on this fallback path. */
static os_mutex g_ref_mu;
static int g_ref_mu_ready = 0;
static void ref_global_init(void) {
  if (!g_ref_mu_ready) {
    os_mutex_init(&g_ref_mu);
    g_ref_mu_ready = 1;
  }
}
static void ref_lock(void) { os_mutex_lock(&g_ref_mu); }
static void ref_unlock(void) { os_mutex_unlock(&g_ref_mu); }

#endif

static void tool_destroy(astools_tool *t) {
  if (!t) return;
  astools_manifest_free(t->m);
  free(t->pkg_dir);
  astools_semver_free(&t->ver);
  free(t);
}

void astools_tool_ref(astools_tool *t) {
  if (!t) return;
  ref_lock();
  t->refcount++;
  ref_unlock();
}

void astools_tool_unref(astools_tool *t) {
  int n;
  if (!t) return;
  ref_lock();
  n = --t->refcount;
  ref_unlock();
  if (n <= 0) tool_destroy(t);
}

/* ---- shared predicates --------------------------------------------------- */

/* §4.1 package-relative: contains a path separator, is not absolute (no
 * leading separator, no drive prefix), and has no ".." component. */
static bool pkg_relative(const char *p) {
  size_t i = 0;
  bool sep = false;
  if (!p || p[0] == '\0') return false;
  if (p[0] == '/' || p[0] == '\\') return false;
  if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
      p[1] == ':')
    return false; /* Windows drive prefix */
  if (os_path_is_abs(p)) return false;
  for (;;) {
    size_t n = 0;
    while (p[i + n] != '\0' && p[i + n] != '/' && p[i + n] != '\\') n++;
    if (n == 2 && p[i] == '.' && p[i + 1] == '.') return false;
    if (p[i + n] == '\0') break;
    sep = true;
    i += n + 1;
  }
  return sep;
}

static int cmp_name(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---- scan fingerprint (§4.3) --------------------------------------------- */

static uint64_t fnv_step(uint64_t h, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  size_t i;
  for (i = 0; i < len; i++) {
    h ^= p[i];
    h *= UINT64_C(0x100000001b3);
  }
  return h;
}

/* FNV-1a over (path, mtime) of every root and every package dir; subdirs
 * are visited in sorted order so the value is stable across scans. */
static uint64_t scan_fp_compute(const astools_config *cfg) {
  uint64_t h = UINT64_C(0xcbf29ce484222325);
  size_t r, i;
  for (r = 0; r < cfg->reg_paths_len; r++) {
    const char *root = cfg->reg_paths[r].path;
    os_stat_info st;
    char **names = NULL;
    size_t n = 0;
    int64_t mt;
    if (!root) continue;
    memset(&st, 0, sizeof st);
    if (os_stat(root, &st) != ASTOOLS_OK) st.mtime_unix = 0;
    h = fnv_step(h, root, strlen(root) + 1);
    mt = st.mtime_unix;
    h = fnv_step(h, &mt, sizeof mt);
    if (os_list_subdirs(root, &names, &n) != ASTOOLS_OK) continue;
    if (n > 1) qsort(names, n, sizeof *names, cmp_name);
    for (i = 0; i < n; i++) {
      char *pkg = os_path_join(root, names[i]);
      if (pkg) {
        memset(&st, 0, sizeof st);
        if (os_stat(pkg, &st) != ASTOOLS_OK) st.mtime_unix = 0;
        h = fnv_step(h, pkg, strlen(pkg) + 1);
        mt = st.mtime_unix;
        h = fnv_step(h, &mt, sizeof mt);
        free(pkg);
      }
      free(names[i]);
    }
    free(names);
  }
  return h;
}

/* ---- candidate construction ---------------------------------------------- */

typedef struct {
  astools_tool **v;
  size_t n, cap;
} tool_vec;

static astools_err vec_push(tool_vec *tv, astools_tool *t) {
  if (tv->n == tv->cap) {
    size_t nc = tv->cap ? tv->cap * 2 : 8;
    astools_tool **nv = realloc(tv->v, nc * sizeof *nv);
    if (!nv) return ASTOOLS_ERR_NOMEM;
    tv->v = nv;
    tv->cap = nc;
  }
  tv->v[tv->n++] = t;
  return ASTOOLS_OK;
}

/* Parse one package dir into a descriptor, or NULL with a WARN log for
 * every reject (§4.2: never fatal). A missing manifest.xcdn means "not a
 * package" and is skipped silently. */
static astools_tool *load_package(astools_ctx *c, const astools_root *root,
                                  const char *pkg_dir) {
  char *mp, *text = NULL, *err_msg = NULL;
  size_t len = 0, i;
  astools_manifest *m;
  astools_tool *t;
  bool plat_ok;
  astools_err e;

  mp = os_path_join(pkg_dir, "manifest.xcdn");
  if (!mp) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "out of memory while scanning '%s'", pkg_dir);
    return NULL;
  }
  e = os_read_file(mp, &text, &len);
  free(mp);
  if (e == ASTOOLS_ERR_NOT_FOUND) return NULL;
  if (e != ASTOOLS_OK) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "cannot read manifest in '%s' (%s)", pkg_dir,
                astools_err_name(e));
    return NULL;
  }

  m = astools_manifest_parse(text, len, c->workspace, &err_msg);
  free(text);
  if (!m) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry", "rejected package '%s': %s",
                pkg_dir, err_msg ? err_msg : "invalid manifest");
    free(err_msg);
    return NULL;
  }

  /* §4.1 trust gates */
  if (m->kind == ASTOOLS_KIND_LIBRARY && root->trust != ASTOOLS_TRUST_FULL) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "rejected package '%s': kind \"library\" requires a "
                "full-trust root",
                pkg_dir);
    goto reject;
  }
  for (i = 0; i < m->entries_len; i++) {
    const char *a0 = m->entries[i].argv_len > 0 ? m->entries[i].argv[0] : NULL;
    if (!a0 || a0[0] == '\0') {
      astools_log(c, ASTOOLS_LOG_WARN, "registry",
                  "rejected package '%s': runtime entry with empty argv",
                  pkg_dir);
      goto reject;
    }
    if (root->trust != ASTOOLS_TRUST_FULL && !pkg_relative(a0)) {
      astools_log(c, ASTOOLS_LOG_WARN, "registry",
                  "rejected package '%s': entry argv[0] '%s' must be "
                  "package-relative in a standard-trust root",
                  pkg_dir, a0);
      goto reject;
    }
  }

  t = calloc(1, sizeof *t);
  if (!t) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "out of memory while scanning '%s'", pkg_dir);
    goto reject;
  }
  if (!astools_semver_parse(m->version, &t->ver)) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "rejected package '%s': version '%s' is not SemVer 2.0.0",
                pkg_dir, m->version);
    free(t);
    goto reject;
  }
  t->pkg_dir = astools_strdup(pkg_dir);
  if (!t->pkg_dir) {
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "out of memory while scanning '%s'", pkg_dir);
    astools_semver_free(&t->ver);
    free(t);
    goto reject;
  }
  t->m = m;
  t->trust = root->trust;
  t->refcount = 1; /* the table's ref */

  plat_ok = (m->platforms_len == 0);
  for (i = 0; i < m->platforms_len; i++)
    if (m->platforms[i] && strcmp(m->platforms[i], astools_plat_os()) == 0)
      plat_ok = true;
  for (i = 0; i < m->entries_len && !t->entry; i++) {
    const astools_entry *en = &m->entries[i];
    if (en->os && en->arch && strcmp(en->os, astools_plat_os()) == 0 &&
        strcmp(en->arch, astools_plat_arch()) == 0)
      t->entry = en;
  }
  t->available = plat_ok && t->entry != NULL;
  return t;

reject:
  astools_manifest_free(m);
  return NULL;
}

/* ---- table comparison (§4.3 list_changed) -------------------------------- */

static char *tool_key(const astools_tool *t) {
  astools_buf b;
  astools_buf_init(&b);
  if (astools_buf_printf(&b, "%s@%s:%d:%s", t->m->id, t->m->version,
                         t->enabled ? 1 : 0,
                         t->pkg_dir ? t->pkg_dir : "") != ASTOOLS_OK) {
    astools_buf_free(&b);
    return NULL;
  }
  return astools_buf_detach(&b);
}

/* Sorted "id@version:enabled:pkg_dir" multiset comparison; allocation
 * failure degrades toward "changed" (a spurious refresh is harmless). */
static int tables_differ(astools_tool *const *a, size_t an,
                         astools_tool *const *b, size_t bn) {
  char **ka = NULL, **kb = NULL;
  size_t i;
  int differ = 0;

  if (an != bn) return 1;
  if (an == 0) return 0;
  ka = calloc(an, sizeof *ka);
  kb = calloc(bn, sizeof *kb);
  if (!ka || !kb) {
    differ = 1;
    goto done;
  }
  for (i = 0; i < an; i++) {
    ka[i] = tool_key(a[i]);
    kb[i] = tool_key(b[i]);
    if (!ka[i] || !kb[i]) {
      differ = 1;
      goto done;
    }
  }
  qsort(ka, an, sizeof *ka, cmp_name);
  qsort(kb, bn, sizeof *kb, cmp_name);
  for (i = 0; i < an; i++) {
    if (strcmp(ka[i], kb[i]) != 0) {
      differ = 1;
      break;
    }
  }
done:
  if (ka)
    for (i = 0; i < an; i++) free(ka[i]);
  if (kb)
    for (i = 0; i < bn; i++) free(kb[i]);
  free(ka);
  free(kb);
  return differ;
}

/* ---- lock path ----------------------------------------------------------- */

/* Relative lock_path resolves against the config dir (internal.h §4.4). */
static char *lock_path_abs(const astools_config *cfg) {
  const char *lp =
      cfg->lock_path && cfg->lock_path[0] ? cfg->lock_path : "astools.lock.xcdn";
  if (os_path_is_abs(lp) || !cfg->config_dir || !cfg->config_dir[0])
    return astools_strdup(lp);
  return os_path_join(cfg->config_dir, lp);
}

/* ---- scan ---------------------------------------------------------------- */

astools_err astools_registry_scan(astools_ctx *c, int *out_changed) {
  tool_vec tv = {NULL, 0, 0};
  astools_lockfile lf = {NULL, 0};
  char *lpath = NULL;
  astools_tool **old = NULL;
  size_t old_n = 0, r, i, j;
  size_t n_enabled = 0, n_unavail = 0;
  uint64_t fp;
  int changed;
  astools_err e = ASTOOLS_OK;

  if (out_changed) *out_changed = 0;
  if (!c) return ASTOOLS_ERR_INVALID;
  ref_global_init();

  /* phase 1: candidate list, no locks (cfg is immutable after open) */
  for (r = 0; r < c->cfg.reg_paths_len && e == ASTOOLS_OK; r++) {
    astools_root abs_root;
    const astools_root *root = &c->cfg.reg_paths[r];
    char **names = NULL;
    char *canon = NULL;
    size_t n = 0;
    if (!root->path) continue;
    /* Roots may be given relative to the process cwd; package dirs must
     * come out absolute because tool argv[0] is resolved against pkg_dir
     * while the child runs with cwd = scratch. */
    if (os_realpath(root->path, &canon) == ASTOOLS_OK) {
      abs_root.path = canon;
      abs_root.trust = root->trust;
      root = &abs_root;
    }
    if (os_list_subdirs(root->path, &names, &n) != ASTOOLS_OK) {
      free(canon);
      continue;
    }
    if (n > 1) qsort(names, n, sizeof *names, cmp_name);
    for (i = 0; i < n; i++) {
      if (e == ASTOOLS_OK) {
        char *pkg = os_path_join(root->path, names[i]);
        if (!pkg) {
          e = ASTOOLS_ERR_NOMEM;
        } else {
          astools_tool *t = load_package(c, root, pkg);
          if (t) {
            bool dup = false;
            for (j = 0; j < tv.n; j++) {
              if (strcmp(tv.v[j]->m->id, t->m->id) == 0 &&
                  astools_semver_cmp(&tv.v[j]->ver, &t->ver) == 0) {
                dup = true;
                break;
              }
            }
            if (dup) {
              astools_log(c, ASTOOLS_LOG_WARN, "registry",
                          "duplicate tool %s@%s in '%s' ignored "
                          "(first root wins)",
                          t->m->id, t->m->version, pkg);
              tool_destroy(t);
            } else if (vec_push(&tv, t) != ASTOOLS_OK) {
              tool_destroy(t);
              e = ASTOOLS_ERR_NOMEM;
            }
          }
          free(pkg);
        }
      }
      free(names[i]);
    }
    free(names);
    free(canon);
  }
  if (e != ASTOOLS_OK) goto fail;

  /* phase 2: lockfile policy (§4.4) */
  lpath = lock_path_abs(&c->cfg);
  if (!lpath) {
    e = ASTOOLS_ERR_NOMEM;
    goto fail;
  }
  e = astools_lockfile_load(lpath, &lf);
  if (e == ASTOOLS_ERR_NOMEM) goto fail;
  if (e != ASTOOLS_OK) {
    /* corrupt lockfile: treat as empty — under "enforce" everything is
     * then unlisted and disabled, which fails closed */
    astools_log(c, ASTOOLS_LOG_WARN, "registry",
                "lockfile '%s' unreadable (%s); treating as empty", lpath,
                astools_err_name(e));
    e = ASTOOLS_OK;
  }
  for (i = 0; i < tv.n; i++) {
    astools_tool *t = tv.v[i];
    t->lock_state =
        astools_lockfile_check(&lf, t->m->id, t->m->version, t->pkg_dir, t->m);
    t->enabled = t->available && (c->cfg.pinning != ASTOOLS_PIN_ENFORCE ||
                                  t->lock_state == ASTOOLS_LOCK_OK);
    if (c->cfg.pinning == ASTOOLS_PIN_WARN && t->available &&
        t->lock_state != ASTOOLS_LOCK_OK)
      astools_log(c, ASTOOLS_LOG_WARN, "registry",
                  "lockfile: %s@%s %s (pinning=warn, tool stays enabled)",
                  t->m->id, t->m->version,
                  t->lock_state == ASTOOLS_LOCK_MISMATCH
                      ? "does not match its recorded hashes"
                      : "is not recorded");
  }

  /* phase 3: fingerprint of the tree just visited */
  fp = scan_fp_compute(&c->cfg);

  /* phase 4: publish atomically */
  os_rwlock_wrlock(&c->lock);
  for (i = 0; i < tv.n; i++) {
    astools_tool *t = tv.v[i];
    for (j = 0; j < c->tools_n; j++) {
      const astools_tool *o = c->tools[j];
      if (strcmp(o->m->id, t->m->id) == 0 &&
          astools_semver_cmp(&o->ver, &t->ver) == 0) {
        t->host_disabled = o->host_disabled; /* sticky across refresh */
        break;
      }
    }
    if (t->host_disabled) t->enabled = false;
  }
  changed = tables_differ(c->tools, c->tools_n, tv.v, tv.n);
  old = c->tools;
  old_n = c->tools_n;
  c->tools = tv.v;
  c->tools_n = tv.n;
  c->tools_cap = tv.cap;
  astools_lockfile_free(&c->lockfile);
  c->lockfile = lf;
  lf.entries = NULL;
  lf.len = 0;
  c->scan_fingerprint = fp;
  c->last_scan_mono = astools_mono(c);
  c->last_refresh_unix = astools_clock_now(&c->clock);
  for (i = 0; i < c->tools_n; i++) {
    if (c->tools[i]->enabled) n_enabled++;
    if (!c->tools[i]->available) n_unavail++;
  }
  c->stats.tools_total = c->tools_n;
  c->stats.tools_enabled = n_enabled;
  c->stats.tools_unavailable = n_unavail;
  c->stats.last_refresh_unix = c->last_refresh_unix;
  if (changed) c->tools_changed_flag = true;
  os_rwlock_wrunlock(&c->lock);

  /* release replaced descriptors outside the lock */
  for (i = 0; i < old_n; i++) astools_tool_unref(old[i]);
  free(old);
  free(lpath);

  if (out_changed) *out_changed = changed;
  return ASTOOLS_OK;

fail:
  for (i = 0; i < tv.n; i++) tool_destroy(tv.v[i]);
  free(tv.v);
  astools_lockfile_free(&lf);
  free(lpath);
  return astools_seterr(c, e, "registry scan failed (%s)",
                        astools_err_name(e));
}

/* ---- resolution (§3.7, §5.1 step 1) -------------------------------------- */

/* Bare-ref ordering: any release beats any pre-release; within the same
 * class the higher SemVer wins. */
static bool tool_better(const astools_tool *cand, const astools_tool *cur) {
  bool cand_rel = cand->ver.prerelease == NULL;
  bool cur_rel = cur->ver.prerelease == NULL;
  if (cand_rel != cur_rel) return cand_rel;
  return astools_semver_cmp(&cand->ver, &cur->ver) > 0;
}

astools_err astools_registry_resolve(astools_ctx *c, const char *ref,
                                     astools_tool **out) {
  const char *at;
  char *id = NULL;
  const char *verstr = NULL;
  bool pinned = false, have_want = false;
  astools_semver want;
  astools_tool *hit = NULL;
  astools_tool *blocked = NULL;
  bool platform_missing = false, id_seen = false;
  char *blocked_ver = NULL;
  bool blocked_host = false;
  astools_lock_state blocked_lock = ASTOOLS_LOCK_UNLISTED;
  size_t i;
  astools_err e;

  if (!out) return ASTOOLS_ERR_INVALID;
  *out = NULL;
  if (!c) return ASTOOLS_ERR_INVALID;
  if (!ref || ref[0] == '\0')
    return astools_seterr(c, ASTOOLS_ERR_INVALID, "resolve: empty tool ref");

  at = strchr(ref, '@');
  if (at) {
    id = astools_strndup(ref, (size_t)(at - ref));
    verstr = at + 1;
  } else {
    id = astools_strdup(ref);
  }
  if (!id) return astools_seterr(c, ASTOOLS_ERR_NOMEM, "out of memory");
  if (id[0] == '\0' || (verstr && verstr[0] == '\0')) {
    free(id);
    return astools_seterr(c, ASTOOLS_ERR_NOT_FOUND,
                          "resolve: malformed ref '%s' (want \"id\" or "
                          "\"id@version\")",
                          ref);
  }

  if (!verstr) {
    /* config pins override bare refs (§3.7) */
    for (i = 0; i < c->cfg.pins_len; i++) {
      if (c->cfg.pins[i].tool && c->cfg.pins[i].version &&
          strcmp(c->cfg.pins[i].tool, id) == 0) {
        verstr = c->cfg.pins[i].version;
        pinned = true;
        break;
      }
    }
  }
  if (verstr) {
    if (!astools_semver_parse(verstr, &want)) {
      e = astools_seterr(c, ASTOOLS_ERR_NOT_FOUND,
                         "resolve: '%s' in %sref '%s' is not a valid SemVer "
                         "version",
                         verstr, pinned ? "the config pin for " : "", ref);
      free(id);
      return e;
    }
    have_want = true;
  }

  os_rwlock_rdlock(&c->lock);
  for (i = 0; i < c->tools_n; i++) {
    astools_tool *t = c->tools[i];
    if (strcmp(t->m->id, id) != 0) continue;
    id_seen = true;
    if (have_want && astools_semver_cmp(&t->ver, &want) != 0) continue;
    if (t->available && t->enabled) {
      if (!hit || tool_better(t, hit)) hit = t;
    } else if (t->available) {
      if (!blocked || tool_better(t, blocked)) blocked = t;
    } else {
      platform_missing = true;
    }
  }
  if (hit) {
    astools_tool_ref(hit); /* pinned before the lock is released */
  } else if (blocked) {
    /* snapshot: `blocked` may be swapped out after the lock drops */
    blocked_ver = astools_strdup(blocked->m->version);
    blocked_host = blocked->host_disabled;
    blocked_lock = blocked->lock_state;
  }
  os_rwlock_rdunlock(&c->lock);

  if (have_want) astools_semver_free(&want);

  if (hit) {
    *out = hit;
    free(id);
    free(blocked_ver);
    return ASTOOLS_OK;
  }
  if (blocked) {
    const char *bv = blocked_ver ? blocked_ver : "?";
    if (blocked_host)
      e = astools_seterr(c, ASTOOLS_ERR_DENIED,
                         "tool '%s@%s' is disabled by the host; re-enable it "
                         "with astools_tool_enable",
                         id, bv);
    else
      e = astools_seterr(c, ASTOOLS_ERR_DENIED,
                         "tool '%s@%s' is blocked by registry.pinning="
                         "\"enforce\" (%s); approve it with "
                         "astools-check --approve %s@%s",
                         id, bv,
                         blocked_lock == ASTOOLS_LOCK_MISMATCH
                             ? "lockfile hash mismatch"
                             : "not in the lockfile",
                         id, bv);
  } else if (platform_missing) {
    e = astools_seterr(c, ASTOOLS_ERR_NOT_FOUND,
                       "tool '%s' is not available on this platform (%s-%s)",
                       id, astools_plat_os(), astools_plat_arch());
  } else if (id_seen) {
    e = astools_seterr(c, ASTOOLS_ERR_NOT_FOUND,
                       "tool '%s' has no version %s in the registry%s", id,
                       verstr ? verstr : "?",
                       pinned ? " (requested by a config pin)" : "");
  } else {
    e = astools_seterr(c, ASTOOLS_ERR_NOT_FOUND,
                       "no tool named '%s' in the registry", id);
  }
  free(id);
  free(blocked_ver);
  return e;
}

/* ---- poll (§4.3) --------------------------------------------------------- */

int astools_registry_poll_due(astools_ctx *c) {
  int64_t last;
  uint64_t fp;
  int due;

  if (!c || !c->cfg.watch_poll) return 0;
  os_rwlock_rdlock(&c->lock);
  last = c->last_scan_mono;
  os_rwlock_rdunlock(&c->lock);
  if (astools_mono(c) - last < c->cfg.poll_interval_ms) return 0;
  /* recompute with no locks held: touches only cfg roots + dir listings */
  fp = scan_fp_compute(&c->cfg);
  os_rwlock_rdlock(&c->lock);
  due = (fp != c->scan_fingerprint);
  os_rwlock_rdunlock(&c->lock);
  return due;
}
