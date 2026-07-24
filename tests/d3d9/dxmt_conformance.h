/* Per-function control for the Wine conformance modules.
 *
 * The build wraps every test call in a module's START_TEST as DXMT_RUN(fn).
 * With no environment set the module runs exactly as it does upstream, so a
 * native Windows run of the same binary is unchanged. The suite uses the two
 * variables below to re-run a module that did not survive its first attempt.
 *
 *   DXMT_TEST_SKIP=a,b,c   do not run these functions
 *   DXMT_TEST_ONLY=a       run only this one
 *
 * Each function announces itself before it runs. A crash is attributed by
 * Wine's own exception filter, which prints the failing file and line, but a
 * HANG prints nothing at all, so without the marker there is no way to name the
 * function that stopped a module. It is flushed immediately because a crash
 * discards whatever is still buffered.
 *
 * These select which test function runs. They do not alter DXMT's behavior and
 * are absent from src/, so the suite still measures the same runtime a plain
 * run would.
 */

#ifndef DXMT_CONFORMANCE_H
#define DXMT_CONFORMANCE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Matches one name inside a comma-separated list, so "test_ab" is not found in
 * a list that only contains "test_abc". */
static int dxmt_conformance_listed(const char *list, const char *name) {
  size_t length = strlen(name);
  const char *cursor = list;

  while (*cursor) {
    const char *separator = strchr(cursor, ',');
    size_t span = separator ? (size_t)(separator - cursor) : strlen(cursor);
    if (span == length && strncmp(cursor, name, length) == 0)
      return 1;
    if (!separator)
      break;
    cursor = separator + 1;
  }
  return 0;
}

static int dxmt_conformance_should_run(const char *name) {
  const char *only = getenv("DXMT_TEST_ONLY");
  const char *skip = getenv("DXMT_TEST_SKIP");

  if (only && *only && !dxmt_conformance_listed(only, name))
    return 0;
  if (skip && *skip && dxmt_conformance_listed(skip, name))
    return 0;
  return 1;
}

#define DXMT_RUN(fn)                                                           \
  do {                                                                         \
    if (dxmt_conformance_should_run(#fn)) {                                     \
      printf("dxmt-conformance: running %s\n", #fn);                            \
      fflush(stdout);                                                           \
      fn();                                                                     \
    }                                                                          \
  } while (0)

#endif /* DXMT_CONFORMANCE_H */
