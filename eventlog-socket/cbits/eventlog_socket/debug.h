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

#define DEBUG_VERBOSITY_QUIET 0
#define DEBUG_VERBOSITY_ERROR 1
#define DEBUG_VERBOSITY_WARN 2
#define DEBUG_VERBOSITY_INFO 3
#define DEBUG_VERBOSITY_DEBUG 4
#define DEBUG_VERBOSITY_TRACE 5

#ifdef DEBUG_VERBOSITY
#else
#ifdef DEBUG
#define DEBUG_VERBOSITY DEBUG_VERBOSITY_TRACE
#else
#define DEBUG_VERBOSITY DEBUG_VERBOSITY_ERROR
#endif /* DEBUG */
#endif /* DEBUG_VERBOSITY */

#define DEBUG_TRACE(fmt, ...)                                                  \
  do {                                                                         \
    if (DEBUG_VERBOSITY >= DEBUG_VERBOSITY_TRACE) {                            \
      fprintf(stderr,                                                          \
              DEBUG_COLOR_PURPLE "TRACE[%s|%d|%s]: " DEBUG_COLOR_RESET fmt     \
                                 "\n",                                         \
              __FILE__, __LINE__, __func__, __VA_ARGS__);                      \
    }                                                                          \
  } while (0)

#define DEBUG_DEBUG(fmt, ...)                                                  \
  do {                                                                         \
    if (DEBUG_VERBOSITY >= DEBUG_VERBOSITY_DEBUG) {                            \
      fprintf(stderr,                                                          \
              DEBUG_COLOR_BLUE "DEBUG[%s|%d|%s]: " DEBUG_COLOR_RESET fmt "\n", \
              __FILE__, __LINE__, __func__, __VA_ARGS__);                      \
    }                                                                          \
  } while (0)

#define DEBUG_INFO(fmt, ...)                                                   \
  do {                                                                         \
    if (DEBUG_VERBOSITY >= DEBUG_VERBOSITY_INFO) {                             \
      fprintf(stderr,                                                          \
              DEBUG_COLOR_YELLOW "INFO[%s|%d|%s]: " DEBUG_COLOR_RESET fmt      \
                                 "\n",                                         \
              __FILE__, __LINE__, __func__, __VA_ARGS__);                      \
    }                                                                          \
  } while (0)

#define DEBUG_WARN(fmt, ...)                                                   \
  do {                                                                         \
    if (DEBUG_VERBOSITY >= DEBUG_VERBOSITY_WARN) {                             \
      fprintf(stderr,                                                          \
              DEBUG_COLOR_YELLOW "WARN[%s|%d|%s]: " DEBUG_COLOR_RESET fmt      \
                                 "\n",                                         \
              __FILE__, __LINE__, __func__, __VA_ARGS__);                      \
    }                                                                          \
  } while (0)

#define DEBUG_ERROR(fmt, ...)                                                  \
  do {                                                                         \
    if (DEBUG_VERBOSITY >= DEBUG_VERBOSITY_ERROR) {                            \
      fprintf(stderr,                                                          \
              DEBUG_COLOR_RED "ERROR[%s|%d|%s]: " DEBUG_COLOR_RESET fmt "\n",  \
              __FILE__, __LINE__, __func__, __VA_ARGS__);                      \
    }                                                                          \
  } while (0)

#define DEBUG_ERRNO(msg)                                                       \
  do {                                                                         \
    if (DEBUG_VERBOSITY >= DEBUG_VERBOSITY_ERROR) {                            \
      perror(DEBUG_COLOR_RED "ERROR: " DEBUG_COLOR_RESET msg);                 \
    }                                                                          \
  } while (0)

#endif /* EVENTLOG_SOCKET_DEBUG_H */
