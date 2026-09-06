/* execution common — runtime implementation. */
#include "execution.h"

void astools_exec_result_clear(astools_result *r) {
  if (!r) return;
  free(r->result_xcdn);
  free(r->error_code);
  free(r->error_message);
  r->result_xcdn = NULL;
  r->error_code = NULL;
  r->error_message = NULL;
  r->ok = 0;
}

/* Engine-produced tool-visible error (reserved astools/... codes). Keeps
 * exit_code/duration untouched; strdup failures degrade to NULL fields. */
void astools_exec_result_set(astools_result *r, const char *code, const char *fmt, ...) {
  va_list ap;
  char msg[512];
  if (!r) return;
  astools_exec_result_clear(r);
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  r->error_code = astools_strdup(code);
  r->error_message = astools_strdup(msg);
}

/* Sleep without a dedicated primitive in os.h: timed wait on a private
 * condition variable nobody signals. */
void astools_exec_sleep(int64_t ms) {
  os_mutex mu;
  os_cond cv;
  if (ms <= 0) return;
  os_mutex_init(&mu);
  os_cond_init(&cv);
  os_mutex_lock(&mu);
  (void)os_cond_timedwait(&cv, &mu, ms);
  os_mutex_unlock(&mu);
  os_cond_destroy(&cv);
  os_mutex_destroy(&mu);
}
