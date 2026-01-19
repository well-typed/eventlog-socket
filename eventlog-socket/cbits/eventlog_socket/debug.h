/* Source: https://github.com/bytebutt/c-debug-header */
/* License: UNLICENSE */

#ifndef EVENTLOG_SOCKET_DEBUG_H
#define EVENTLOG_SOCKET_DEBUG_H

#include <stdio.h>

#ifdef DEBUG_COLOR /* Colorize with ANSI escape sequences. */

#define DEBUG_COLOR_RED "[31m"
#define DEBUG_COLOR_GREEN "[32m"
#define DEBUG_COLOR_YELLOW "[33m"
#define DEBUG_COLOR_BLUE "[34m"
#define DEBUG_COLOR_PURPLE "[35m"
#define DEBUG_COLOR_RESET "[m"

#else

#define DEBUG_COLOR_RED ""
#define DEBUG_COLOR_GREEN ""
#define DEBUG_COLOR_YELLOW ""
#define DEBUG_COLOR_BLUE ""
#define DEBUG_COLOR_PURPLE ""
#define DEBUG_COLOR_RESET ""

#endif /* DEBUG_COLOR */

#define DEBUG_PREFIX(tag, color)                                               \
  printf(color "%s[%s|%d|%s]: " DEBUG_COLOR_RESET, tag, __FILE__, __LINE__,    \
         __func__)

#ifdef DEBUG

#define DEBUG_TRACE(...)                                                       \
  do {                                                                         \
    DEBUG_PREFIX("TRACE", DEBUG_COLOR_BLUE);                                   \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
  } while (0)
#define DEBUG_WARN(...)                                                        \
  do {                                                                         \
    DEBUG_PREFIX("WARNING", DEBUG_COLOR_YELLOW);                               \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
  } while (0)
#define DEBUG_ERROR(...)                                                       \
  do {                                                                         \
    DEBUG_PREFIX("ERROR", DEBUG_COLOR_RED);                                    \
    fprintf(stderr, __VA_ARGS__);                                              \
    fprintf(stderr, "\n");                                                     \
  } while (0)
#define DEBUG_ERRNO(s)                                                         \
  do {                                                                         \
    DEBUG_PREFIX("ERROR", DEBUG_COLOR_RED);                                    \
    perror(s);                                                                 \
  } while (0)

#else

#define DEBUG_TRACE(...) ((void)0)
#define DEBUG_WARN(...) ((void)0)
#define DEBUG_ERROR(...) ((void)0)
#define DEBUG_ERRNO(s) ((void)0)

#endif /* DEBUG */
#endif /* EVENTLOG_SOCKET_DEBUG_H */
