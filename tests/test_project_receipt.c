#include "astools_test.h"
#include "../std/project_receipt.h"
TEST(junit_counts_and_abstention) {
  long tests, skipped, failed;
  ASSERT_EQ_INT(astd_junit_counts("<testsuite name=\"suite\" tests=\"2\" skipped=\"1\" failures=\"0\"></testsuite>", &tests, &skipped, &failed), 0);
  ASSERT_EQ_INT(tests, 2); ASSERT_EQ_INT(skipped, 1); ASSERT_EQ_INT(failed, 0);
  ASSERT_EQ_INT(astd_junit_counts("<testsuite tests=\"0\" failures=\"0\"></testsuite>", &tests, &skipped, &failed), 0);
  ASSERT_EQ_INT(tests, 0);
  ASSERT_EQ_INT(astd_junit_counts("<testsuite tests=\"1\" skipped=\"1\"></testsuite>", &tests, &skipped, &failed), 0);
  ASSERT_EQ_INT(tests, skipped);
  ASSERT_TRUE(astd_junit_counts("exit_code: 0, tests: 1", &tests, &skipped, &failed) != 0);
  ASSERT_TRUE(astd_junit_counts("<testsuite tests=\"1\">", &tests, &skipped, &failed) != 0);
  ASSERT_TRUE(astd_junit_counts("<testsuite tests=\"-1\"></testsuite>", &tests, &skipped, &failed) != 0);
  ASSERT_TRUE(astd_junit_counts("<testsuite tests=\"1\" skipped=\"2\"></testsuite>", &tests, &skipped, &failed) != 0);
}
TEST_LIST = {TEST_ENTRY(junit_counts_and_abstention)};
RUN_ALL_TESTS()
