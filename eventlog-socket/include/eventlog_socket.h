#ifndef EVENGLOG_SOCKET_H
#define EVENGLOG_SOCKET_H

#include <Rts.h>
#include <rts/EventLogWriter.h>

/* eventlog socket configuration
 *********************************************************************************/

// eventlog-socket supports two address families.
typedef enum {
  // Unix domain socket
  EVENTLOG_UNIX,
  // TCP/IPv4
  EVENTLOG_INET,
} eventlog_socket_addr_family_t;

typedef struct {
  const char *path;
} eventlog_unix_socket_t;

typedef struct {
  const char *host;
  const char *port;
} eventlog_inet_socket_t;

typedef struct {
  const eventlog_socket_addr_family_t addr_family;
  const union {
    const eventlog_unix_socket_t unix_socket;
    const eventlog_inet_socket_t inet_socket;
  } addr;
} eventlog_socket_t;

extern const EventLogWriter SocketEventLogWriter;

void eventlog_socket_init_unix(const char *sock_path);
void eventlog_socket_init_tcp(const char *host, const char *port);
void eventlog_socket_ready(void);

void eventlog_socket_wait(void);

void eventlog_socket_start_unix(const char *sock_path, bool wait);
void eventlog_socket_start_tcp(const char *host, const char *port, bool wait);

int eventlog_socket_hs_main(int argc, char *argv[], RtsConfig conf, StgClosure *main_closure);

#endif
