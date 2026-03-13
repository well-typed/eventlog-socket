#include <netdb.h>
#include <string.h>

#include "./string.h"
#include "eventlog_socket.h"

#define STRERROR_BUFLEN_INIT 1024

/* PUBLIC - see documentation in eventlog_socket.h */
char *eventlog_socket_strerror(EventlogSocketStatus status) {
  switch (status.ess_status_code) {
  case EVENTLOG_SOCKET_OK: {
    break;
  }
  case EVENTLOG_SOCKET_ERR_RTS_NOSUPPORT: {
    return es_strdup("The GHC RTS does not support the eventlog.");
  }
  case EVENTLOG_SOCKET_ERR_RTS_FAIL: {
    return es_strdup("The GHC RTS could not start the eventlog writer.");
  }
  case EVENTLOG_SOCKET_ERR_ENV_NOADDR: {
    return es_strdup("No socket address was found in the environment.");
  }
  case EVENTLOG_SOCKET_ERR_ENV_TOOLONG: {
    return es_strdup("The Unix domain socket path found was too long.");
  }
  case EVENTLOG_SOCKET_ERR_ENV_NOHOST: {
    return es_strdup(
        "A TCP/IP port number was found, but no host name was found.");
  }
  case EVENTLOG_SOCKET_ERR_ENV_NOPORT: {
    return es_strdup(
        "A TCP/IP host name was found, but no port number was found.");
  }
  case EVENTLOG_SOCKET_ERR_CTL_NOSUPPORT: {
    return es_strdup(
        "This binary was compiled without support for control commands.");
  }
  case EVENTLOG_SOCKET_ERR_CTL_EXISTS: {
    return es_strdup(
        "The requested namespace or command ID is already in use.");
  }
  case EVENTLOG_SOCKET_ERR_GAI: {
    return es_strdup(gai_strerror(status.ess_error_code));
  }
  case EVENTLOG_SOCKET_ERR_SYS: {
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
  }
  errno = EINVAL;
  return NULL;
}
