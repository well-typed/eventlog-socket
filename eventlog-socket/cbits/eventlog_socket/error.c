#include <netdb.h>
#include <string.h>

#include "eventlog_socket.h"
#include "./string.h"

#define STRERROR_BUFLEN_INIT 1024

/* PUBLIC - see documentation in eventlog_socket.h */
char *eventlog_socket_strerror(EventlogSocketStatus status) {
  switch (status.ess_status_code) {
  case EVENTLOG_SOCKET_OK: {
    return ess_strdup("Ok");
  }
  case EVENTLOG_SOCKET_ERROR_RTS_NOSUPPORT: {
    return ess_strdup(
        "This version of the GHC RTS does not support the eventlog.");
  }
  case EVENTLOG_SOCKET_ERROR_RTS_FAIL: {
    return ess_strdup("The GHC RTS could not start the eventlog writer.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_NOADDR: {
    return ess_strdup("No socket address was found in the environment.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_TOOLONG: {
    return ess_strdup("The Unix domain socket path found was too long.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_NOHOST: {
    return ess_strdup(
        "A TCP/IP port number was found, but no host name was found.");
  }
  case EVENTLOG_SOCKET_ERROR_CNF_NOPORT: {
    return ess_strdup(
        "A TCP/IP host name was found, but no port number was found.");
  }
  case EVENTLOG_SOCKET_ERROR_CMD_EXISTS: {
    return ess_strdup("The requested combination of namespace and command ID "
                        "is already in use.");
  }
  case EVENTLOG_SOCKET_ERROR_GAI: {
    return ess_strdup(gai_strerror(status.ess_error_code));
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
