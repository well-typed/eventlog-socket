#include "eventlog_socket.h"
#include <netdb.h>
#include <string.h>

#define STRERROR_BUFLEN_INIT 1024

/// @brief Copy the first `str_len` characters of a string into allocated
/// memory.
///
/// @return Upon successful completion, a pointer to an allocated copy of the
/// string is returned.
///
/// @return On error, a null pointer is returned and errno is set to indicate
/// the error.
static char *strcpy_alloc(const char *const str) {
  if (str == NULL) {
    errno = EINVAL;
    return NULL;
  }
  char *str_copy = calloc(strlen(str) + 1, sizeof(char));
  if (str_copy == NULL) {
    return NULL; // `calloc` sets errno.
  }
  strcpy(str_copy, str);
  return str_copy;
}

/* PUBLIC - see documentation in eventlog_socket.h */
char *eventlog_socket_strerror(EventlogSocketStatus status) {
  switch (status.ess_status_code) {
  case EVENTLOG_SOCKET_OK: {
    return strcpy_alloc("Ok");
  }
  case EVENTLOG_SOCKET_ERROR_RTS_NOSUPPORT: {
    return strcpy_alloc(
        "This version of the GHC RTS does not support the eventlog.");
  }
  case EVENTLOG_SOCKET_ERROR_RTS_FAIL: {
    return strcpy_alloc("The GHC RTS could not start the eventlog writer.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_NOADDR: {
    return strcpy_alloc("No socket address was found in the environment.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_TOOLONG: {
    return strcpy_alloc("The Unix domain socket path found was too long.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_NOHOST: {
    return strcpy_alloc(
        "A TCP/IP port number was found, but no host name was found.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_NOPORT: {
    return strcpy_alloc(
        "A TCP/IP host name was found, but no port number was found.");
  }
  case EVENTLOG_SOCKET_ERROR_CMD_EXISTS: {
    return strcpy_alloc("The requested combination of namespace and command ID "
                        "is already in use.");
  }
  case EVENTLOG_SOCKET_ERROR_GAI: {
    return strcpy_alloc(gai_strerror(status.ess_error_code));
  }
  case EVENTLOG_SOCKET_ERROR_SYSTEM: {
    size_t buflen = STRERROR_BUFLEN_INIT;
    char *strerrbuf = strerrbuf = malloc(buflen);
    if (strerrbuf == NULL) {
      return NULL; // `malloc` sets errno.
    }
    while (strerror_r(status.ess_error_code, strerrbuf, buflen) == -1 &&
           errno == ERANGE) {
      // Double the buffer size and try again.
      buflen *= 2;
      char *strerrbuf_new = realloc(strerrbuf, buflen);
      if (strerrbuf_new == NULL) {
        free(strerrbuf);
        return NULL; // `realloc` sets errno.
      }
      strerrbuf = strerrbuf_new;
    }
    return strerrbuf;
  }
  default: {
    errno = EINVAL;
    return NULL;
  }
  }
}
