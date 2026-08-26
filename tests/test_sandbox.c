/*
 * test_sandbox.c — sandbox limit selection.
 *
 * RLIMIT_NPROC is charged per real UID across the whole system, not per
 * process tree, so the cap a tool is spawned with must clear the task count
 * the account already runs. A fixed cap denied fork() outright to every
 * tool on a host that was merely busy, while staying invisible on an idle
 * CI runner.
 */

#include "astools_test.h"

#include "astools.h"
#include "astools_internal.h"
#include "os.h"

TEST(nproc_cap_clears_current_usage) {
  int64_t used = 0, cap = 0;

  if (os_proc_user_tasks(&used) != ASTOOLS_OK) {
    /* No per-uid task count here: no cap may be claimed either. */
    ASSERT_EQ_INT(astools_sandbox_nproc_cap(&cap), ASTOOLS_ERR_UNSUPPORTED);
    return;
  }

  ASSERT_TRUE(used >= 1); /* this process is itself a task */
  ASSERT_OK(astools_sandbox_nproc_cap(&cap));
  /* The regression: true on an idle runner with a handful of tasks and on
   * a desktop session with thousands. A cap at or below current usage
   * makes the tool's first fork() fail with EAGAIN. */
  ASSERT_TRUE(cap > used);
  ASSERT_EQ_INT((int)(cap - used), ASTOOLS_NPROC_HEADROOM);
}

/* The capability report must not claim a process cap the runtime would not
 * actually be able to apply. */
TEST(process_cap_claim_matches_reality) {
  astools_sandbox_caps caps;
  int64_t cap = 0;
  int can_cap;

  memset(&caps, 0, sizeof caps);
  ASSERT_OK(astools_sandbox_caps_impl(0, &caps));
  can_cap = (astools_sandbox_nproc_cap(&cap) == ASTOOLS_OK);
#if defined(__linux__)
  ASSERT_EQ_INT(caps.process_cap, can_cap);
#elif defined(_WIN32)
  ASSERT_EQ_INT(caps.process_cap, 1);
  ASSERT_EQ_INT(can_cap, 0); /* Job cap has no per-user-count helper. */
#else
  ASSERT_EQ_INT(caps.process_cap, 0);
  (void)can_cap;
#endif
}

TEST_LIST = {
  TEST_ENTRY(nproc_cap_clears_current_usage),
  TEST_ENTRY(process_cap_claim_matches_reality),
};

RUN_ALL_TESTS()
