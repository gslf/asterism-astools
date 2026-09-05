/* Cancellable admission and asynchronous invocation ownership. */
#include "astools_internal.h"
#include <stdlib.h>
#include <string.h>

int astools_task_cancelled(astools_task *t) {
  int cancelled;
  if (!t) return 0;
  os_mutex_lock(&t->mu);
  cancelled = t->cancelled ? 1 : 0;
  os_mutex_unlock(&t->mu);
  return cancelled;
}

/* ---- slot admission (invocation.max_concurrent) -------------------------- */

astools_err astools_slot_acquire(astools_ctx *c, int64_t deadline, astools_task *cancel) {
  if (!c) return ASTOOLS_ERR_INVALID;
  int max = c->cfg.max_concurrent > 0 ? c->cfg.max_concurrent : 1;
  astools_err e = ASTOOLS_OK;
  bool waiting = false;
  os_mutex_lock(&c->slot_mu);
  for (;;) {
    if (astools_task_cancelled(cancel)) {
      e = ASTOOLS_ERR_CANCELLED;
      break;
    }
    int64_t now = astools_mono(c);
    if (deadline > 0 && now >= deadline) {
      e = ASTOOLS_ERR_TIMEOUT;
      break;
    }
    if (c->slots_used < max) {
      c->slots_used++;
      break;
    }
    if (c->no_threads) {
      e = ASTOOLS_ERR_BUSY;
      break;
    }
    if (!waiting) {
      waiting = true;
      c->slots_waiting++;
    }
    int64_t wait_ms = deadline > 0 && deadline - now < 50 ? deadline - now : 50;
    (void)os_cond_timedwait(&c->slot_cv, &c->slot_mu, wait_ms);
  }
  if (waiting) c->slots_waiting--;
  os_mutex_unlock(&c->slot_mu);
  return e;
}

void astools_slot_release(astools_ctx *c) {
  if (!c) return;
  os_mutex_lock(&c->slot_mu);
  if (c->slots_used > 0) c->slots_used--;
  os_cond_signal(&c->slot_cv);
  os_mutex_unlock(&c->slot_mu);
}

/* ---- async tasks (astools_invoke_async) ----------------------------------- */

#if !defined(ASTOOLS_NO_THREADS)
static void *task_main(void *arg) {
  astools_task *t = arg;
  astools_result res;
  astools_err v = astools_invoke_impl(t->c, t->ref, t->command, t->args, t->deadline_ms, t,
                                      t->checked ? t->expected_sha256 : NULL, &res);
  os_mutex_lock(&t->mu);
  t->result = res;
  t->verdict = v;
  t->done = true;
  os_cond_broadcast(&t->cv);
  os_mutex_unlock(&t->mu);
  return NULL;
}
#endif

astools_err astools_invoke_async_checked(astools_ctx *c, const char *ref, const char *command,
                                         const char *args_xcdn, uint32_t deadline_ms,
                                         const uint8_t *expected_sha256, astools_task **out) {
  if (out) *out = NULL;
  if (!c || !ref || !command || !out) return ASTOOLS_ERR_INVALID;
#if defined(ASTOOLS_NO_THREADS)
  (void)args_xcdn;
  (void)deadline_ms;
  (void)expected_sha256;
  return astools_seterr(c, ASTOOLS_ERR_UNSUPPORTED,
                        "async invocation needs threads "
                        "(ASTOOLS_NO_THREADS build)");
#else
  if (c->no_threads)
    return astools_seterr(c, ASTOOLS_ERR_UNSUPPORTED,
                          "async invocation is unavailable in "
                          "no-thread mode");
  {
    astools_task *t = calloc(1, sizeof *t);
    astools_err e;
    if (!t) return ASTOOLS_ERR_NOMEM;
    t->c = c;
    if (expected_sha256) {
      t->checked = true;
      memcpy(t->expected_sha256, expected_sha256, 32);
    }
    t->ref = astools_strdup(ref);
    t->command = astools_strdup(command);
    t->args = astools_strdup(args_xcdn); /* NULL-safe */
    t->deadline_ms = deadline_ms;
    os_mutex_init(&t->mu);
    os_cond_init(&t->cv);
    if (!t->ref || !t->command || (args_xcdn && !t->args)) {
      e = ASTOOLS_ERR_NOMEM;
      goto fail;
    }
    e = os_thread_start(&t->thread, task_main, t);
    if (e != ASTOOLS_OK) {
      (void)astools_seterr(c, e, "cannot start invocation thread");
      goto fail;
    }
    t->thread_valid = true;
    *out = t;
    return ASTOOLS_OK;
  fail:
    os_mutex_destroy(&t->mu);
    os_cond_destroy(&t->cv);
    free(t->ref);
    free(t->command);
    free(t->args);
    free(t);
    return e;
  }
#endif
}

astools_err astools_invoke_async(astools_ctx *c, const char *ref, const char *command,
                                 const char *args, uint32_t deadline_ms, astools_task **out) {
  return astools_invoke_async_checked(c, ref, command, args, deadline_ms, NULL, out);
}

int astools_task_done(astools_task *t) {
  if (!t) return 0;
  os_mutex_lock(&t->mu);
  int done = t->done;
  os_mutex_unlock(&t->mu);
  return done;
}

astools_err astools_task_wait(astools_task *t, uint32_t timeout_ms, astools_result *out) {
  int64_t end;
  astools_err v;
  if (!t) return ASTOOLS_ERR_INVALID;
  end = os_monotonic_ms() + (int64_t)timeout_ms;
  os_mutex_lock(&t->mu);
  while (!t->done) {
    int64_t rem = end - os_monotonic_ms();
    if (rem <= 0) {
      os_mutex_unlock(&t->mu);
      return ASTOOLS_ERR_BUSY;
    }
    (void)os_cond_timedwait(&t->cv, &t->mu, rem);
  }
  if (out) {
    /* Ownership of the result strings transfers on the first delivery;
     * later waits observe the same verdict/scalars with NULL strings. */
    *out = t->result;
    t->result.result_xcdn = NULL;
    t->result.error_code = NULL;
    t->result.error_message = NULL;
  }
  v = t->verdict;
  os_mutex_unlock(&t->mu);
  return v;
}

astools_err astools_task_cancel(astools_task *t) {
  if (!t) return ASTOOLS_ERR_INVALID;
  os_mutex_lock(&t->mu);
  t->cancelled = true;
  os_mutex_unlock(&t->mu);
  return ASTOOLS_OK;
}

void astools_task_free(astools_task *t) {
  if (!t) return;
  os_mutex_lock(&t->mu);
  t->cancelled = true; /* unblock a still-running invocation */
  os_mutex_unlock(&t->mu);
  if (t->thread_valid) {
    os_thread_join(&t->thread);
    t->thread_valid = false;
  }
  free(t->ref);
  free(t->command);
  free(t->args);
  free(t->result.result_xcdn);
  free(t->result.error_code);
  free(t->result.error_message);
  os_mutex_destroy(&t->mu);
  os_cond_destroy(&t->cv);
  free(t);
}
