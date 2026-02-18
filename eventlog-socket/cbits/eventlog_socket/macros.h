#ifndef EVENTLOG_SOCKET_MACROS_H
#define EVENTLOG_SOCKET_MACROS_H

#include <stdlib.h>

// Portable macro for marking definitions as hidden.
#ifndef HIDDEN
#ifdef __has_attribute
#if __has_attribute(visibility)
#define HIDDEN __attribute__((visibility("hidden")))
#endif
#endif
#ifndef HIDDEN
#define HIDDEN ((void)0)
#endif
#endif

// Portable macro for marking cases with explicit fallthrough.
#ifndef FALLTHROUGH
#ifdef __has_attribute
#if __has_attribute(fallthrough)
#define FALLTHROUGH __attribute__((fallthrough))
#endif
#endif
#ifndef FALLTHROUGH
#define FALLTHROUGH ((void)0)
#endif
#endif

#endif /* EVENTLOG_SOCKET_MACROS_H */
