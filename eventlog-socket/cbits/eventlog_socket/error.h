#ifndef EVENTLOG_SOCKET_ERR_H
#define EVENTLOG_SOCKET_ERR_H

#include <stdlib.h>

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

/// @brief If the @p expr returns an error status, immediately return it.
#define RETURN_ON_ERROR(expr)                                                  \
  do {                                                                         \
    const EventlogSocketStatus status = (expr);                                \
    if (status.ess_status_code != EVENTLOG_SOCKET_OK) {                        \
      return status;                                                           \
    }                                                                          \
  } while (0)

#endif /* EVENTLOG_SOCKET_ERR_H */
