#ifndef ASTOOLS_PROJECT_RECEIPT_H
#define ASTOOLS_PROJECT_RECEIPT_H
/* Returns 0 only for a complete recognized JUnit suite summary. */
int astd_junit_counts(const char *xml, long *tests, long *skipped, long *failed);
#endif
