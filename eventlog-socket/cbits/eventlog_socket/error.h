#ifndef EVENTLOG_SOCKET_ERR_H
#define EVENTLOG_SOCKET_ERR_H

#include "./debug.h"

/// @brief Construct a status from an `EventlogSocketStatusCode` status code.
#define STATUS_FROM_CODE(status_code)                                          \
  ((EventlogSocketStatus){.ess_status_code = (status_code),                    \
                          .ess_error_code = 0})

/// @brief Construct a status from `errno`.
#define STATUS_FROM_ERRNO()                                                    \
  ((EventlogSocketStatus){.ess_status_code = EVENTLOG_SOCKET_ERR_SYS,          \
                          .ess_error_code = errno})

/// @brief Construct a status from a `pthread.h` error code.
#define STATUS_FROM_PTHREAD_ERROR(pthread_errno)                               \
  ((EventlogSocketStatus){.ess_status_code = EVENTLOG_SOCKET_ERR_SYS,          \
                          .ess_error_code = (pthread_errno)})

/// @brief Construct a status from a `getaddrinfo` error code.
#define STATUS_FROM_GAI_ERROR(gai_error)                                       \
  ((EventlogSocketStatus){.ess_status_code = EVENTLOG_SOCKET_ERR_GAI,          \
                          .ess_error_code = (gai_error)})

/// @brief A condition that evaluates to @c true if @p expr is an error.
///
/// @warning This macro evaluates its first argument multiple times.
#define STATUS_IS_ERROR(expr)                                                  \
  /* If none of the following are true... */                                   \
  (!((/* ...the status is an OK. */                                            \
      (expr).ess_status_code == EVENTLOG_SOCKET_OK) ||                         \
     (/* ...the status is a successful getaddrinfo exit code. */               \
      (expr).ess_status_code == EVENTLOG_SOCKET_ERR_GAI &&                     \
      (expr).ess_error_code == 0) ||                                           \
     (/* ...the status is a successful system call exit code. */               \
      (expr).ess_status_code == EVENTLOG_SOCKET_ERR_SYS &&                     \
      (expr).ess_error_code == 0)))

/// @brief If the @p expr returns an error status, immediately return it.
#define RETURN_ON_ERROR(expr)                                                  \
  do {                                                                         \
    const EventlogSocketStatus status = (expr);                                \
    if (STATUS_IS_ERROR(status)) {                                             \
      char *strerr = eventlog_socket_strerror(status);                         \
      DEBUG_ERROR("%s", strerr);                                               \
      free(strerr);                                                            \
      return status;                                                           \
    }                                                                          \
  } while (0)

#endif /* EVENTLOG_SOCKET_ERR_H */
