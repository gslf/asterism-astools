/* Bound instance residency and acquire ordered streams with cancellable waits. */
#include "execution.h"

void astools_pp_node_free(astools_pproc *p) {
  if (!p) return;
  os_mutex_destroy(&p->io_mu);
  astools_buf_free(&p->pending);
  astools_tool_unref(p->tool);
  free(p);
}

static void instance_key(int kind, const astools_tool *t, char key[65]) {
  astools_sha256_ctx h;
  uint8_t digest[32];
  astools_sha256_init(&h);
  astools_sha256_update(&h, t->pkg_dir, strlen(t->pkg_dir) + 1);
  /* dlopen may retain a loaded image for the same path: reject its replacement. */
  if (kind == PP_KIND_PROC) astools_sha256_update(&h, t->content_sha256, 32);
  astools_sha256_final(&h, digest);
  for (size_t i = 0; i < 32; i++)
    snprintf(key + i * 2, 3, "%02x", digest[i]);
}

astools_err astools_pp_acquire(astools_ctx *c, int kind, astools_tool *t, int64_t deadline,
                               astools_task *cancel, astools_pproc **out) {
  char key[65];
  int limit = t->m->parallel > 0 ? t->m->parallel : 1;
  *out = NULL;
  instance_key(kind, t, key);
  os_mutex_lock(&c->pp_mu);
  astools_err e = ASTOOLS_OK;
  bool waiting = false;
  for (;;) {
    astools_pproc *chosen = NULL;
    size_t count = 0, total = 0;
    if (astools_task_cancelled(cancel)) {
      e = ASTOOLS_ERR_CANCELLED;
      break;
    }
    if (deadline && astools_mono(c) >= deadline) {
      e = ASTOOLS_ERR_TIMEOUT;
      break;
    }
    for (astools_pproc *p = c->pprocs; p; p = p->next) {
      total++;
      if (p->kind != kind || strcmp(p->key, key)) continue;
      count++;
      if (!p->busy || kind == PP_KIND_LIB) chosen = p;
    }
    if (!chosen && count < (size_t)limit) {
      if (total >= ASTOOLS_INSTANCE_LIMIT) {
        e = ASTOOLS_ERR_BUSY;
        break;
      }
      chosen = calloc(1, sizeof *chosen);
      if (!chosen) {
        e = ASTOOLS_ERR_NOMEM;
        break;
      }
      chosen->kind = kind;
      memcpy(chosen->key, key, sizeof key);
      chosen->tool = t;
      astools_tool_ref(t);
      os_mutex_init(&chosen->io_mu);
      chosen->proc.fd_in = chosen->proc.fd_out = chosen->proc.fd_err = -1;
      chosen->next = c->pprocs;
      c->pprocs = chosen;
      c->pp_count++;
    }
    if (chosen) {
      chosen->busy++;
      *out = chosen;
      break;
    }
    if (!waiting) {
      waiting = true;
      c->pp_waiters++;
    }
    int64_t wait = deadline - astools_mono(c);
    if (wait > 50 || !deadline) wait = 50;
    if (wait > 0) os_cond_timedwait(&c->pp_cv, &c->pp_mu, wait);
  }
  if (waiting) c->pp_waiters--;
  os_mutex_unlock(&c->pp_mu);
  return e;
}

void astools_pp_release(astools_ctx *c, astools_pproc *p) {
  os_mutex_lock(&c->pp_mu);
  if (p->busy > 0) p->busy--;
  p->last_used_mono = astools_mono(c);
  os_cond_broadcast(&c->pp_cv);
  os_mutex_unlock(&c->pp_mu);
}

/* Stop/reap the leader and its remaining process group before deleting scratch. */
void astools_pp_stop(astools_ctx *c, astools_pproc *p, bool graceful) {
  int ec = 0;
  if (graceful && p->rpc_lines) {
    os_proc_close_stdin(&p->proc);
    if (os_proc_wait(&p->proc,200,&ec) == ASTOOLS_OK) graceful = false;
  }
  if (graceful) {
    os_proc_terminate(&p->proc);
    if (os_proc_wait(&p->proc, 2000, &ec) != ASTOOLS_OK) {
      os_proc_kill(&p->proc);
      (void)os_proc_wait(&p->proc, -1, &ec);
    }
  } else {
    os_proc_kill(&p->proc);
    (void)os_proc_wait(&p->proc, -1, &ec);
  }
  os_proc_kill(&p->proc); /* A cooperative leader may leave children behind. */
  os_proc_free(&p->proc);
  memset(&p->proc, 0, sizeof p->proc);
  p->proc.fd_in = p->proc.fd_out = p->proc.fd_err = -1;
  p->alive = p->hello_done = false;
  astools_buf_free(&p->pending);
  astools_sandbox_cleanup(c, &p->setup, false);
}

void astools_pp_backoff(astools_ctx *c, astools_pproc *p) {
  p->backoff_ms = p->backoff_ms > 0 ? p->backoff_ms * 2 : 1000;
  if (p->backoff_ms > 30000) p->backoff_ms = 30000;
  p->next_restart_mono = astools_mono(c) + p->backoff_ms;
}

/* Recheck after the per-instance queue, which can outlive global admission. */
astools_err astools_pp_validate(astools_ctx *c, const astools_tool *t) {
  char ref[160];
  astools_tool *current = NULL;
  snprintf(ref, sizeof ref, "%s@%s", t->m->id, t->m->version);
  astools_err e = astools_registry_resolve(c, ref, &current);
  if (e == ASTOOLS_OK && strcmp(current->pkg_dir, t->pkg_dir)) e = ASTOOLS_ERR_DENIED;
  if (e == ASTOOLS_OK) e = astools_registry_check_snapshot(c, current, t->content_sha256);
  if (e == ASTOOLS_OK) e = astools_registry_revalidate(c, current);
  astools_tool_unref(current);
  return e;
}
