/* Wine's debug string helpers are inline in wine/debug.h and call back into the
 * two entry points below, which Wine's own loader exports. A standalone PE has
 * no such loader, so the corpus links against nothing and these have to be
 * supplied here rather than by editing the upstream tests.
 *
 * The tests only use the results to print diagnostics, so a rotating set of
 * buffers is enough: the returned string has to outlive the printf that formats
 * it and nothing keeps one longer than a few calls. */

#include <stdio.h>
#include <string.h>

#ifndef __cdecl
#define __cdecl
#endif

#define WINE_DBG_BUFFER_COUNT 16
#define WINE_DBG_BUFFER_SIZE 512

const char *__cdecl __wine_dbg_strdup(const char *str) {
  static char buffers[WINE_DBG_BUFFER_COUNT][WINE_DBG_BUFFER_SIZE];
  static unsigned int next;
  char *buffer = buffers[next++ % WINE_DBG_BUFFER_COUNT];
  if (str == NULL)
    return "(null)";
  snprintf(buffer, WINE_DBG_BUFFER_SIZE, "%s", str);
  return buffer;
}

int __cdecl __wine_dbg_output(const char *str) {
  return fputs(str, stderr);
}
