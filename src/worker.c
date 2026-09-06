/* worker — runtime implementation. */
#include "execution.h"

/* ---- supervisor ------------------------------------------------------ */

/* One pass of background work: registry poll + idle persistent reaping. */
static void worker_pass(astools_ctx *c) {
  if (astools_registry_poll_due(c)) {
    int changed = 0;
    (void)astools_registry_scan(c, &changed);
  }
  /* Pin idle nodes before examining registry state: never invert lock order. */
  astools_pproc *idle[ASTOOLS_INSTANCE_LIMIT];
  size_t n = 0;
  os_mutex_lock(&c->pp_mu);
  for (astools_pproc *p = c->pprocs; p && n < ASTOOLS_INSTANCE_LIMIT; p = p->next)
    if (p->kind == PP_KIND_PROC && !p->busy) {
      p->busy = 1;
      idle[n++] = p;
    }
  os_mutex_unlock(&c->pp_mu);
  for (size_t i = 0; i < n; i++) {
    astools_pproc *p = idle[i];
    bool expired =
        p->idle_timeout_ms > 0 && astools_mono(c) - p->last_used_mono >= p->idle_timeout_ms;
    bool revoked = astools_pp_validate(c, p->tool) != ASTOOLS_OK;
    if (expired || revoked) {
      astools_pp_stop(c, p, !revoked);
      os_mutex_lock(&c->pp_mu);
      astools_pproc **link = &c->pprocs;
      while (*link && *link != p)
        link = &(*link)->next;
      if (*link) {
        *link = p->next;
        c->pp_count--;
      }
      os_cond_broadcast(&c->pp_cv);
      os_mutex_unlock(&c->pp_mu);
      astools_pp_node_free(p);
    } else {
      os_mutex_lock(&c->pp_mu);
      p->busy = 0; /* Supervision is not use: preserve the idle deadline. */
      os_cond_broadcast(&c->pp_cv);
      os_mutex_unlock(&c->pp_mu);
    }
  }
}

#if !defined(ASTOOLS_NO_THREADS)
static void *worker_main(void *arg) {
  astools_ctx *c = arg;
  os_mutex_lock(&c->ev_mu);
  while (!c->stop_worker) {
    (void)os_cond_timedwait(&c->ev_cv, &c->ev_mu, 1000);
    if (c->stop_worker) break;
    os_mutex_unlock(&c->ev_mu);
    worker_pass(c);
    os_mutex_lock(&c->ev_mu);
  }
  os_mutex_unlock(&c->ev_mu);
  return NULL;
}
#endif

astools_err astools_worker_start(astools_ctx *c) {
  if (!c) return ASTOOLS_ERR_INVALID;
#if defined(ASTOOLS_NO_THREADS)
  c->worker_running = false;
  return ASTOOLS_OK; /* tick-driven (astools_tick) */
#else
  if (c->no_threads || c->worker_running) return ASTOOLS_OK;
  c->stop_worker = false;
  {
    astools_err e = os_thread_start(&c->worker, worker_main, c);
    if (e != ASTOOLS_OK) return astools_seterr(c, e, "cannot start supervisor thread");
  }
  c->worker_running = true;
  return ASTOOLS_OK;
#endif
}

void astools_worker_stop(astools_ctx *c) {
  astools_pproc *p, *next;
  if (!c) return;
  if (c->worker_running) {
    os_mutex_lock(&c->ev_mu);
    c->stop_worker = true;
    os_cond_broadcast(&c->ev_cv);
    os_mutex_unlock(&c->ev_mu);
    os_thread_join(&c->worker);
    c->worker_running = false;
  }
  /* No invocations are in flight at close; drain the table. */
  os_mutex_lock(&c->pp_mu);
  p = c->pprocs;
  c->pprocs = NULL;
  c->pp_count = 0;
  os_mutex_unlock(&c->pp_mu);
  for (; p; p = next) {
    next = p->next;
    if (p->kind == PP_KIND_PROC) {
      astools_pp_stop(c, p, true);
    } else if (p->lib_loaded) {
      if (p->lib_init_ok && p->vt && p->vt->shutdown) p->vt->shutdown();
      os_dylib_close(&p->lib);
    }
    astools_pp_node_free(p);
  }
}

astools_err astools_worker_tick(astools_ctx *c) {
  if (!c) return ASTOOLS_ERR_INVALID;
  worker_pass(c);
  return ASTOOLS_OK;
}
