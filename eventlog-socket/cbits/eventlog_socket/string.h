#ifndef EVENTLOG_SOCKET_STRING_H
#define EVENTLOG_SOCKET_STRING_H

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "./macros.h"

/// @brief Copy the @p str into allocated memory.
///
/// @return Upon successful completion, a pointer to an allocated copy of @p str
/// is returned.
///
/// @return On error, a null pointer is returned and errno is set to indicate
/// the error.
HIDDEN char *ess_strdup(const char *str);

/// @brief Copy the first @p str_len bytes of @p str into allocated memory.
///
/// @return Upon successful completion, a pointer to an allocated copy of a
/// null-terminated copy of the first @p str_len bytes of @p str is returned.
///
/// @return On error, a null pointer is returned and errno is set to indicate
/// the error.
HIDDEN char *ess_strndup(size_t str_len, const char str[str_len + 1]);

#endif /* EVENTLOG_SOCKET_STRING_H */
